#include "timeline_decoder.hpp"

#include "sync_constants.hpp"

#include "canvas/core/gpu/cuda_convert.hpp"
#include "canvas/core/timeline/clip_rate.hpp"
#include "canvas/core/util/color_log.hpp"
#include "canvas/core/util/log.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>

namespace canvas::gui {

// Forward declarations: the seq→media mappers are defined with the other
// file-local helpers below but used from the decode entry points earlier.
namespace {
double media_fps_of(const canvas::core::Project& project, const canvas::core::Clip& clip);
int64_t seq_to_src_frame(const canvas::core::Project& project, const canvas::core::Clip& clip,
                         int64_t seq_frame);
// Shared NV12 staging for the slot decode path and the off-thread transition
// bake (defined below with the other helpers).
canvas::core::Nv12FramePtr host_nv12_from_hw(const AVFrame* hw, std::int64_t src_frame,
                                             const canvas::core::gpu::ColorSpec& spec,
                                             int max_dim);

// The most recent decode_nv12()/decode_nv12_slot() null reason, published at
// each early return and consumed by frame()/preview() right after an NV12
// attempt fails, so the CPU-RGBA fallback line says WHY it ran. The ~1Hz
// full-res decode() with 195ms-5.4s stalls in the logs is this fallback, and
// without the reason it looks like a random re-decode. thread_local: the
// playback worker, transition-bake thread and tests interleave, but the reason
// is always consumed on the same thread that set it.
thread_local const char* g_last_nv12_null_reason = "never-tried";
thread_local std::int64_t g_last_nv12_null_ms = 0;

// Every-fallback trace: each occurrence is a full-res CPU decode on the
// playback/scrub path (0.4-6s stalls), so throttling would lose the very frames
// being chased. `why`/`nv12_ms` are captured BEFORE the CPU decode runs (from
// the thread-local reason the NV12 attempt published), `decode_ms` after — the
// pair shows whether the stall is the NV12 miss itself or the CPU GOP re-walk.
void trace_rgba_fallback(const char* side, int media, std::int64_t seq_frame,
                         std::int64_t tl_in, const char* why,
                         std::int64_t nv12_ms, double decode_ms) {
    ::canvas::core::log::log_warning(
        "[dec] RGBA-FALLBACK side=%s media=%d seq=%lld tl_in=%lld why=%s "
        "nv12_ms=%.2f decode_ms=%.2f",
        side, media, static_cast<long long>(seq_frame),
        static_cast<long long>(tl_in), why, static_cast<double>(nv12_ms),
        decode_ms);
}

// Full-res decode() attribution. A full-res CPU decode on the playback path is
// almost always frame()'s NV12-fallback (RGBA-FALLBACK) — but the 02:50 log had
// 13 fullres decodes (0.07-1.3s each) with NO RGBA-FALLBACK, implying decode()
// is reached from a context that never attempted the NV12 fast path. Every
// decode() call then logs an ungated FULLRES-CALLED line carrying the origin
// (0=none, 1=frame(), 2=preview()) plus whatever reason the last NV12 attempt
// published, so the next stall run names its caller even without CANVAS_DEBUG.
thread_local int t_decode_origin = 0;
class DecodeOriginGuard {
public:
    explicit DecodeOriginGuard(int context) : saved_(t_decode_origin) {
        t_decode_origin = context;
    }
    ~DecodeOriginGuard() { t_decode_origin = saved_; }

private:
    int saved_;
};
}  // namespace

void TimelineDecoder::add_media(const canvas::core::MediaEntry& entry) {
    auto slot = std::make_unique<DecoderSlot>();
    std::string error;
    if (slot->decoder.open(entry.path, &error, hw_.device_ctx())) slot->loaded = true;
    // Always-on stream census: how many streams the container holds, which one
    // the decoder picked, and every video stream present (so two video streams
    // show up instead of silently being ignored).
    if (slot->loaded) {
        ::canvas::core::log::log_warning(
            "[media] open id=%d hw=%s %s path=%s", entry.id,
            slot->decoder.is_hardware() ? "yes" : "no",
            slot->decoder.video_stream_summary().c_str(), entry.path.c_str());
    } else {
        const char* err = error.empty() ? "unknown" : error.c_str();
        ::canvas::core::log::log_warning("[media] OPEN-FAILED id=%d err=%s path=%s",
                                     entry.id, err, entry.path.c_str());
    }
    slots_[entry.id] = std::move(slot);
    CANVAS_LOG("video: decoded slot media %d loaded=%d hw=%d dims=%dx%d path=%s", entry.id,
           slots_[entry.id]->loaded, slots_[entry.id]->decoder.is_hardware(),
           slots_[entry.id]->decoder.width(), slots_[entry.id]->decoder.height(),
           entry.path.c_str());
}

void TimelineDecoder::open_b_slot(const canvas::core::Project& project,
                                  const canvas::core::Clip& clip) {
    // A true two-clip cross-dissolve on the SAME media needs a second decode
    // position from the same file. The main slot walks A's tail; the B slot
    // walks B's pre-roll independently, so neither side has to seek back on
    // the shared session (which re-walks a whole GOP per frame — the ~10fps
    // stall + 4.4s A/V drift seen in the first pass at this fix). The two
    // slots share the CUDA device but own separate decode sessions.
    if (b_slots_.count(clip.media) > 0) return;
    const auto it = std::find_if(project.media.begin(), project.media.end(),
                                 [&](const canvas::core::MediaEntry& m) { return m.id == clip.media; });
    if (it == project.media.end()) return;
    auto slot = std::make_unique<DecoderSlot>();
    std::string error;
    if (slot->decoder.open(it->path, &error, hw_.device_ctx())) {
        slot->loaded = true;
        ::canvas::core::log::log_warning(
            "[dec] B-slot open id=%d hw=%s path=%s", clip.media,
            slot->decoder.is_hardware() ? "yes" : "no", it->path.c_str());
    } else {
        ::canvas::core::log::log_warning("[dec] B-slot OPEN-FAILED id=%d err=%s path=%s",
                                         clip.media,
                                         error.empty() ? "unknown" : error.c_str(),
                                         it->path.c_str());
    }
    b_slots_[clip.media] = std::move(slot);
}

void TimelineDecoder::close() {
    // Stop the transition-bake thread FIRST: it holds its own decoder sessions
    // and reads hardware-frames from the shared device, so it must be joined
    // before the slots (and the device) are torn down. bake_stop_ aborts an
    // in-flight bake at the next frame boundary; the worker picks up the result
    // — if any — like any other frame, but close also drops it so nothing stale
    // survives the teardown. bake_thread_ is left non-joinable and the object
    // reusable (tests call add_media after close).
    {
        std::lock_guard<std::mutex> lk(bake_mutex_);
        bake_stop_ = true;
        bake_cv_.notify_all();
    }
    if (bake_thread_.joinable()) bake_thread_.join();
    {
        std::lock_guard<std::mutex> lk(bake_mutex_);
        bake_stop_ = false;  // tests reuse the object after close()
        bake_job_.reset();
        bake_inflight_ = false;
        bake_result_.reset();
    }

    // Project-switch census: how many decoder slots were torn down and how many
    // had gone hardware, so add_media storms (one decode init per media) are
    // attributable to the switch rather than to a fill_lookahead loop.
    size_t hw_slots = 0;
    for (const auto& [id, slot] : slots_)
        if (slot->loaded && slot->decoder.is_hardware()) ++hw_slots;
    ::canvas::core::log::log_warning("[dec] close slots=%zu hw=%zu previews=%zu grade_luts=%zu",
                                 slots_.size(), hw_slots, preview_cache_.size(),
                                 grade_lut_cache_.size());
    slots_.clear();
    b_slots_.clear();
    preview_cache_.clear();
    preview_lru_.clear();
    grade_lut_cache_.clear();
}

void TimelineDecoder::invalidate(const canvas::core::MediaId media) {
    const size_t before_slots = slots_.size();
    slots_.erase(media);
    b_slots_.erase(media);
    for (auto it = preview_cache_.begin(); it != preview_cache_.end();) {
        if (it->first.media == media) {
            preview_lru_.erase(std::remove(preview_lru_.begin(), preview_lru_.end(), it->first),
                               preview_lru_.end());
            it = preview_cache_.erase(it);
        } else {
            ++it;
        }
    }
    ::canvas::core::log::log_warning("[dec] invalidate media=%d slots=%zu->%zu",
                                 (int)media, before_slots, slots_.size());
}

TimelineDecoder::PreviewStats TimelineDecoder::take_preview_stats() {
    const PreviewStats out{preview_hits_, preview_misses_, preview_evictions_};
    preview_hits_ = 0;
    preview_misses_ = 0;
    preview_evictions_ = 0;
    return out;
}

TimelineDecoder::GradeStats TimelineDecoder::take_grade_stats() {
    const GradeStats out{grade_samples_, grade_ms_sum_, grade_ms_max_};
    grade_samples_ = 0;
    grade_ms_sum_ = 0.0;
    grade_ms_max_ = 0.0;
    return out;
}

canvas::core::grade_graph::GradeLutPtr TimelineDecoder::grade_lut_for(
    const canvas::core::Clip& clip) {
    if (!clip.has_grade()) return nullptr;

    const GradeLutKey key{clip.id, clip.grade.change_seq};
    const auto it = grade_lut_cache_.find(key);
    if (it != grade_lut_cache_.end()) return it->second;

    static canvas::core::ClipId engaged_clip_ = 0;
    if (engaged_clip_ != clip.id) {
        engaged_clip_ = clip.id;
        int lgg = 0, curves = 0, other = 0;
        const auto& g = clip.grade;
        for (int i = 0; i < g.num_nodes(); ++i) {
            switch (g.node(i).correct_mode) {
                case canvas::core::grade_graph::CorrectMode::kLgg:
                    ++lgg;
                    break;
                case canvas::core::grade_graph::CorrectMode::kCurves:
                    ++curves;
                    break;
                default:
                    ++other;  // identity/cdl correctors + the output node
                    break;
            }
        }
        ::canvas::core::log::log_info(
            "[grade] engaged clip=%llu nodes=%d lgg=%d curves=%d other=%d (3D LUT path)",
            static_cast<unsigned long long>(clip.id), g.num_nodes(), lgg, curves, other);
    }

    const auto t0 = std::chrono::steady_clock::now();
    // Log the LGG/Offset structs the graph actually feeds the baker — a second
    // source of truth vs. the wheel widget's own commit trace. If these two
    // ever disagree, the scale law or the graph build dropped/reordered a term.
    {
        using namespace canvas::core::grade_graph;
        using namespace canvas::core::colorsci;
        const auto& g = clip.grade;
        bool any = false;
        for (int i = 0; i < g.num_nodes(); ++i) {
            const Node& n = g.node(i);
            if (n.lgg) {
                const LGG& p = *n.lgg;
                ::canvas::core::log::log_info(
                    "[grade] graph-lgg clip=%llu node=%d "
                    "lift=(m=%.4f r=%.4f g=%.4f b=%.4f) "
                    "gamma=(m=%.4f r=%.4f g=%.4f b=%.4f) "
                    "gain=(r=%.4f g=%.4f b=%.4f)",
                    static_cast<unsigned long long>(clip.id), n.id, p.lift_master,
                    p.lift_r, p.lift_g, p.lift_b, p.gamma_master, p.gamma_r, p.gamma_g,
                    p.gamma_b, p.gain_r, p.gain_g, p.gain_b);
                any = true;
            }
            if (n.offset) {
                const Offset& o = *n.offset;
                ::canvas::core::log::log_info(
                    "[grade] graph-offset clip=%llu node=%d off=(m=%.4f r=%.4f g=%.4f b=%.4f)",
                    static_cast<unsigned long long>(clip.id), n.id, o.master, o.r, o.g,
                    o.b);
                any = true;
            }
        }
        if (!any)
            ::canvas::core::log::log_info(
                "[grade] graph-lgg clip=%llu nodes=%d (no LGG/offset node)",
                static_cast<unsigned long long>(clip.id), g.num_nodes());
    }
    canvas::core::grade_graph::GradeLutPtr lut =
        canvas::core::grade_graph::bake_grade_lut(clip.grade);
    const double ms = std::chrono::duration<double, std::milli>(
                          std::chrono::steady_clock::now() - t0)
                          .count();
    if (lut) {
        const auto digest = canvas::core::grade_graph::grade_lut_digest(*lut);
        static std::uint64_t last_hash = 0;
        const bool changed = digest.hash != last_hash;
        if (changed) last_hash = digest.hash;
        ::canvas::core::log::log_info(
            "[grade] LUT-baked clip=%llu seq=%llu t=%llu size=%d bake_ms=%.2f hash=%016llx "
            "mid=(%.3f,%.3f,%.3f) black=(%.3f,%.3f,%.3f) white=(%.3f,%.3f,%.3f) "
            "skin=(%.3f,%.3f,%.3f) maxdev=%.3f changed=%d",
            static_cast<unsigned long long>(clip.id),
            static_cast<unsigned long long>(clip.grade.change_seq),
            static_cast<unsigned long long>(::canvas::core::log::epoch_ms()), lut->size, ms,
            static_cast<unsigned long long>(digest.hash), digest.mid[0], digest.mid[1],
            digest.mid[2], digest.black[0], digest.black[1], digest.black[2],
            digest.white[0], digest.white[1], digest.white[2], digest.skin[0], digest.skin[1],
            digest.skin[2], digest.max_dev, changed ? 1 : 0);
        // Color archive: same bake, correlated with the GUI [grade] commit by
        // seq and the viewer upload by seq — the always-on page log.
        CANVAS_COLOR_LOG(
            "[grade] bake clip=%llu seq=%llu size=%d hash=%016llx "
            "black=(%.3f,%.3f,%.3f) white=(%.3f,%.3f,%.3f) "
            "skin=(%.3f,%.3f,%.3f) maxdev=%.3f changed=%d",
            static_cast<unsigned long long>(clip.id),
            static_cast<unsigned long long>(clip.grade.change_seq), lut->size,
            static_cast<unsigned long long>(digest.hash), digest.black[0], digest.black[1],
            digest.black[2], digest.white[0], digest.white[1], digest.white[2],
            digest.skin[0], digest.skin[1], digest.skin[2], digest.max_dev,
            changed ? 1 : 0);
    }
    grade_lut_cache_.emplace(key, lut);
    return lut;
}

bool TimelineDecoder::is_loaded(const canvas::core::MediaId id) const {
    const auto it = slots_.find(id);
    return it != slots_.end() && it->second->loaded;
}

bool TimelineDecoder::is_hardware(const canvas::core::MediaId id) const {
    const auto it = slots_.find(id);
    return it != slots_.end() && it->second->loaded && it->second->decoder.is_hardware();
}

canvas::core::VideoFramePtr TimelineDecoder::decode(const canvas::core::Project& project,
                                                const canvas::core::Clip& clip,
                                                const std::int64_t seq_frame,
                                                const int max_dim) {
    if (clip.media < 0) return nullptr;
    if (!clip.enabled) return make_black_frame(clip, max_dim);

    auto it = slots_.find(clip.media);
    if (it == slots_.end() || !it->second->loaded) {
        static unsigned missing_slot_ = 0;
        if ((++missing_slot_ & 15u) == 0u) {
            const bool has_slot = slots_.count(clip.media) > 0;
            const bool loaded = it != slots_.end() && it->second->loaded;
            ::canvas::core::log::log_warning("[dec] MISSING-SLOT media=%d seq=%lld "
                                         "has_slot=%d loaded=%d",
                                         clip.media,
                                         static_cast<long long>(seq_frame),
                                         has_slot, loaded);
        }
        return nullptr;
    }
    auto* slot = it->second.get();
    const int64_t src_frame = seq_to_src_frame(project, clip, seq_frame);

    // Fast low-res preview path with its own LRU so a reduced frame never
    // displaces (or is returned as) a full-res playback frame.
    if (max_dim > 0) {
        // Build the I-frame index lazily so random scrub seeks jump straight to
        // the owning keyframe instead of searching the container per seek.
        if (!slot->decoder.has_iframe_index()) slot->decoder.build_iframe_index();
        const PreviewKey key{clip.media, src_frame};
        auto cit = preview_cache_.find(key);
        if (cit != preview_cache_.end()) {
            ++preview_hits_;
            preview_lru_.erase(std::remove(preview_lru_.begin(), preview_lru_.end(), key),
                               preview_lru_.end());
            preview_lru_.push_back(key);
            static int n = 0;
            if (((++n) & 3u) == 0u)
                ::canvas::core::log::log_warning("[scrub] PREVIEW-CACHE HIT media=%d "
                                             "src_frame=%lld seq=%lld",
                                             clip.media,
                                             static_cast<long long>(src_frame),
                                             static_cast<long long>(seq_frame));
            return cit->second;
        }
        auto frame = slot->decoder.decode_to_frame(src_frame, max_dim);
        ++preview_misses_;
        if (frame) {
            static int d = 0;
            if (((++d) & 3u) == 0u)
                ::canvas::core::log::log_warning("[scrub] PREVIEW-DECODE media=%d "
                                             "src_frame=%lld dims=%dx%d",
                                             clip.media,
                                             static_cast<long long>(src_frame),
                                             frame->width, frame->height);
            preview_cache_[key] = frame;
            preview_lru_.push_back(key);
            if (preview_lru_.size() > kPreviewCacheMax) {
                const PreviewKey oldest = preview_lru_.front();
                preview_lru_.pop_front();
                preview_cache_.erase(oldest);
                ++preview_evictions_;
            }
        }
        return frame;
    }

    auto frame = slot->cache.get(src_frame);
    // Full-res decode telemetry: measure the decode cost so playback stalls are
    // attributable (a GOP-backwards hardware indexed-seek vs a forward sequential
    // walk feel very different). The ~1s `[dec]` aggregate reports cache-hit rate
    // and avg/peak decode_ms so a throughput cliff shows up as hit% falling and
    // avg_ms climbing together, not as a mysterious dropped-frame cadence.
    static auto dec_log_at = std::chrono::steady_clock::now();
    static uint64_t fullres_req_ = 0, fullres_hits_ = 0;
    static canvas::core::FrameCache::Stats last_cache_stats_{};
    static canvas::core::VideoDecoder::PathStats last_path_stats_{};
    static double fullres_ms_sum_ = 0.0, fullres_ms_max_ = 0.0;
    bool was_cache_hit = frame != nullptr;
    double dec_ms = 0.0;
    if (!frame) {
        const auto dec_t0 = std::chrono::steady_clock::now();
        frame = slot->decoder.decode_to_frame(src_frame);
        dec_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - dec_t0).count();
        fullres_ms_sum_ += dec_ms;
        if (dec_ms > fullres_ms_max_) fullres_ms_max_ = dec_ms;
        // The decoder labels a frame with its *decoded* PTS-derived number, which can
        // differ from the requested target (a forward-walk holding the first frame
        // at/after the target, or PTS/rate skew). Cache only when the numbers
        // agree: keying the LRU by a wrong number aliases the frame (the playhead
        // would later get frame N when src_frame M was asked for). A miss just
        // re-decodes.
        if (frame && frame->frame_number == src_frame) slot->cache.put(frame);
        ::canvas::core::log::log_warning(
            "[dec] FULLRES-CALLED media=%d seq=%lld src=%lld ms=%.1f hw=%d dims=%dx%d "
            "origin=%d nv12_reason=%s",
            clip.media, static_cast<long long>(seq_frame),
            static_cast<long long>(src_frame), dec_ms,
            (int)slot->decoder.is_hardware(), slot->decoder.width(),
            slot->decoder.height(), t_decode_origin, g_last_nv12_null_reason);
    } else {
        ++fullres_hits_;
    }
    ++fullres_req_;
    const auto dec_now = std::chrono::steady_clock::now();
    if (fullres_req_ == 1 || dec_now - dec_log_at >= std::chrono::seconds(1)) {
        dec_log_at = dec_now;
        const double avg_ms =
            fullres_req_ > 0 ? fullres_ms_sum_ / static_cast<double>(fullres_req_) : 0.0;
        const auto cs = slot->cache.stats();
        // Per-window cache deltas: evict_delta/miss_delta are how many entries
        // were dropped/decoded afresh THIS second. Sustained evict storms with a
        // high budget% mean the frame budget is too small for the timeline
        // (decode-thrash) — the same shape as the older scrub-cache bug.
        const auto evict_delta = cs.evictions < last_cache_stats_.evictions
            ? 0 : cs.evictions - last_cache_stats_.evictions;
        const auto miss_delta = cs.misses < last_cache_stats_.misses
            ? 0 : cs.misses - last_cache_stats_.misses;
        last_cache_stats_ = cs;
        // Seek-vs-sequential path deltas for this window (steady playback must be
        // ~100% sequential): seq_avg/seek_avg are the per-window mean decode cost.
        const auto ps = slot->decoder.path_stats();
        const auto seq_delta = ps.sequential < last_path_stats_.sequential
            ? 0 : ps.sequential - last_path_stats_.sequential;
        const auto seek_delta = ps.seeks < last_path_stats_.seeks
            ? 0 : ps.seeks - last_path_stats_.seeks;
        const auto seq_ms_delta = ps.sequential_ms < last_path_stats_.sequential_ms
            ? 0.0 : ps.sequential_ms - last_path_stats_.sequential_ms;
        const auto seek_ms_delta = ps.seek_ms < last_path_stats_.seek_ms
            ? 0.0 : ps.seek_ms - last_path_stats_.seek_ms;
        last_path_stats_ = ps;
        const double seq_avg = seq_delta > 0 ? seq_ms_delta / static_cast<double>(seq_delta) : 0.0;
        const double seek_avg = seek_delta > 0 ? seek_ms_delta / static_cast<double>(seek_delta) : 0.0;
        const auto conv_delta = ps.convert_ms < last_path_stats_.convert_ms
            ? 0.0 : ps.convert_ms - last_path_stats_.convert_ms;
        const double conv_pct =
            seq_ms_delta > 0.0 ? 100.0 * conv_delta / seq_ms_delta : 0.0;
        const double budget_pct = cs.max_bytes > 0
            ? 100.0 * static_cast<double>(cs.bytes) / static_cast<double>(cs.max_bytes) : 0.0;
        ::canvas::core::log::log_warning(
            "[dec] fullres req=%llu hit=%llu (%.0f%%) avg_ms=%.2f max_ms=%.2f hw=%d "
            "cache_hits=%llu cache_misses=%llu miss_delta=%llu evict_delta=%llu "
            "budget=%.0f%% bytes=%zu/%zu seq=%llu seeks=%llu seq_avg_ms=%.2f seek_avg_ms=%.2f "
            "conv_pct=%.0f%% slots=%zu%s",
            static_cast<unsigned long long>(fullres_req_),
            static_cast<unsigned long long>(fullres_hits_),
            fullres_req_ > 0
                ? 100.0 * static_cast<double>(fullres_hits_) / static_cast<double>(fullres_req_)
                : 0.0,
            avg_ms, fullres_ms_max_, (int)slot->decoder.is_hardware(),
            static_cast<unsigned long long>(cs.hits),
            static_cast<unsigned long long>(cs.misses),
            static_cast<unsigned long long>(miss_delta),
            static_cast<unsigned long long>(evict_delta), budget_pct, cs.bytes, cs.max_bytes,
            static_cast<unsigned long long>(seq_delta),
            static_cast<unsigned long long>(seek_delta), seq_avg, seek_avg,
            conv_pct, slots_.size(), was_cache_hit ? "" : " (decode)");
        fullres_req_ = fullres_hits_ = 0;
        fullres_ms_sum_ = 0.0;
        fullres_ms_max_ = 0.0;
    }
    return frame;
}

canvas::core::VideoFramePtr TimelineDecoder::make_black_frame(const canvas::core::Clip& clip,
                                                          const int max_dim) const {
    int width = 1920;
    int height = 1080;
    if (const auto it = slots_.find(clip.media); it != slots_.end() && it->second->loaded) {
        width = it->second->decoder.width();
        height = it->second->decoder.height();
        if (width <= 0 || height <= 0) {
            width = 1920;
            height = 1080;
        }
    }
    if (max_dim > 0 && (width > max_dim || height > max_dim)) {
        const double scale = static_cast<double>(max_dim) / std::max(width, height);
        width = std::max(1, static_cast<int>(std::llround(width * scale)));
        height = std::max(1, static_cast<int>(std::llround(height * scale)));
    }

    auto frame = std::make_shared<canvas::core::VideoFrame>();
    frame->width = width;
    frame->height = height;
    frame->stride = static_cast<std::size_t>(width) * 4;
    frame->rgba.assign(frame->stride * static_cast<std::size_t>(height), 0);
    return frame;
}

canvas::core::Nv12FramePtr TimelineDecoder::decode_nv12(const canvas::core::Project& project,
                                                     const canvas::core::Clip& clip,
                                                     const std::int64_t seq_frame,
                                                     const int max_dim) {
    if (clip.media < 0) {
        g_last_nv12_null_reason = "media-id-negative";
        return nullptr;
    }
    if (!clip.enabled) {
        g_last_nv12_null_reason = "clip-disabled";
        return nullptr;
    }
    if (!canvas::core::gpu::cuda_available()) {
        g_last_nv12_null_reason = "no-cuda";
        return nullptr;
    }

    auto it = slots_.find(clip.media);
    if (it == slots_.end() || !it->second->loaded) {
        g_last_nv12_null_reason = "slot-missing-or-not-loaded";
        return nullptr;
    }
    return decode_nv12_slot(it->second.get(), project, clip, seq_frame, max_dim);
}

canvas::core::Nv12FramePtr TimelineDecoder::decode_nv12_slot(DecoderSlot* slot,
                                                         const canvas::core::Project& project,
                                                         const canvas::core::Clip& clip,
                                                         const std::int64_t seq_frame,
                                                         const int max_dim) {
    // Build the I-frame index lazily on the GPU path too: decode_to_hw_indexed
    // needs it to anchor a scrub/commit on the owning keyframe. Without it the
    // fallback is a plain container seek + forward decode on every position
    // change (~520ms/scrub observed vs a tens-of-ms one-GOP walk indexed).
    if (!slot->decoder.has_iframe_index()) slot->decoder.build_iframe_index();
    // decode_to_hw serves only the CUDA device and the composite kernel consumes
    // CUDA device pointers, so require hardware decode + a CUDA device.
    if (!slot->decoder.is_hardware() || hw_.device_name() != "cuda") {
        g_last_nv12_null_reason = "not-hardware-or-device-not-cuda";
        return nullptr;
    }

    const int64_t src_frame = seq_to_src_frame(project, clip, seq_frame);
    // Two GPU decode strategies, chosen by path:
    //
    //  Prepared playback (max_dim == 0): sequential-forward when the target is
    //  at-or-ahead of the decoder, so steady frames decode cheaply with no
    //  per-frame container seek + codec flush. Random/backward access falls back
    //  to the keyframe-anchored indexed seek (one GOP).
    //
    //  Scrub preview (max_dim > 0): always keyframe-anchored. The caps keep the
    //  sparse-GOP walk cheap (~17ms measured) and every move — forward or
    //  backward — lands near the target. Never blend sequential mode into a
    //  preview drag: a capped sequential walk parks the decoder behind the
    //  playhead and leaves the preview on the wrong (stale) picture.
    const AVFrame* hw;
    double hw_ms = 0.0;
    const auto hw_t0 = std::chrono::steady_clock::now();
    if (max_dim > 0) {
        hw = slot->decoder.decode_to_hw_indexed(
            src_frame, ::canvas::core::VideoDecoder::kPreviewMaxOver);
    } else {
        // Prepared playback: keep the cheap sequential HW walk for small forward
        // deltas (steady-state warming advances frame-by-frame). A large forward
        // jump must NOT walk sequentially from wherever the decoder sits — a far
        // release-commit would decode every frame between, stalling the worker
        // for seconds and freezing every drag preview queued behind it. Anchor
        // those on the owning I-frame so the walk is bounded by one GOP.
        const int64_t dec_pos = slot->decoder.current_frame();
        if (src_frame >= dec_pos && src_frame - dec_pos <= kCommitSeqMaxDelta) {
            hw = slot->decoder.decode_to_hw(src_frame);
        } else {
            static unsigned indexed_hw_ = 0;
            if ((++indexed_hw_ & 15u) == 0u)
                ::canvas::core::log::log_warning(
                    "[dec] HW-INDEXED src=%lld dec=%lld delta=%lld clip_tl_in=%lld",
                    static_cast<long long>(src_frame),
                    static_cast<long long>(dec_pos),
                    static_cast<long long>(src_frame - dec_pos),
                    static_cast<long long>(clip.tl_in));
            hw = slot->decoder.decode_to_hw_indexed(src_frame);
        }
    }
    hw_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - hw_t0).count();
    if (!hw || !hw->data[0] || !hw->data[1]) {
        g_last_nv12_null_reason = "decode-to-hw-null";
        g_last_nv12_null_ms = static_cast<std::int64_t>(hw_ms);
        return nullptr;
    }

    // Shared NV12 staging with the off-thread transition bake
    // (host_nv12_from_hw), so both paths are pixel-identical: same reduce rule
    // (longest edge capped at max_dim, 0 = native, even dims), same on-GPU
    // resize/download, same resolved color spec. The viewer letterboxes the
    // quad, so the composite needs no bars (dst == full canvas).
    const canvas::core::gpu::ColorSpec spec = slot->decoder.color_spec();
    const auto gpu_t0 = std::chrono::steady_clock::now();
    auto frame = host_nv12_from_hw(hw, src_frame, spec, max_dim);
    if (!frame) {
        g_last_nv12_null_reason = "host-nv12-staging-null";
        g_last_nv12_null_ms = 0;
        return nullptr;
    }
    const double gpu_ms =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - gpu_t0).count();
    const int out_w = frame->width;
    const int out_h = frame->height;
    // NV12 GPU fast-path timing: decode_to_hw (NVDEC) vs the on-GPU resize/composite
    // download, so a GPU-path regression (driver, memory pressure, slice layout)
    // shows up as hw_ms/gpu_ms climbing in the ~1s aggregate.
    static auto nv12_log_at = std::chrono::steady_clock::now();
    static uint64_t nv12_req_ = 0;
    static double nv12_hw_ms_ = 0.0, nv12_gpu_ms_ = 0.0;
    static double nv12_hw_max_ = 0.0, nv12_gpu_max_ = 0.0;
    ++nv12_req_;
    nv12_hw_ms_ += hw_ms;
    nv12_gpu_ms_ += gpu_ms;
    nv12_hw_max_ = std::max(nv12_hw_max_, hw_ms);
    nv12_gpu_max_ = std::max(nv12_gpu_max_, gpu_ms);
    // Per-frame tracing, throttled to every 8th request: the ~1s aggregate line
    // below carries the trend (req/fps_hw/hw_avg/gpu_avg), so this one only
    // needs to sample stray spikes — at 30fps a per-frame line is ~30 lines/s
    // of log that drowns the ~1s [play]/[viewer] health lines.
    if (::canvas::core::log::enabled() && (nv12_req_ & 7u) == 0)
        ::canvas::core::log::log_warning(
            "[dec] nv12 media=%d seq=%lld src=%lld max_dim=%d dec_hw_ms=%.2f gpu_ms=%.2f",
            clip.media, static_cast<long long>(seq_frame),
            static_cast<long long>(src_frame), max_dim, hw_ms, gpu_ms);
    const auto nv12_now = std::chrono::steady_clock::now();
    if (nv12_req_ == 1 || nv12_now - nv12_log_at >= std::chrono::seconds(1)) {
        const double elaps_s = std::max(1e-3, std::chrono::duration<double>(nv12_now - nv12_log_at).count());
        nv12_log_at = nv12_now;
        const double avg_hw = nv12_hw_ms_ / static_cast<double>(nv12_req_);
        const double avg_gpu = nv12_gpu_ms_ / static_cast<double>(nv12_req_);
        // fps_hw is the hw-decode+resize cadence (timeline frames/s). It never
        // beating the timeline fps while [play] cadence looks normal means the
        // GPU compositor is the cap; beating it comfortably means the presenter
        // (or decode granularity) is.
        ::canvas::core::log::log_warning(
            "[dec] nv12 req=%llu fps_hw=%.1f hw_avg_ms=%.2f hw_max_ms=%.2f gpu_avg_ms=%.2f gpu_max_ms=%.2f "
            "dims=%dx%d",
            static_cast<unsigned long long>(nv12_req_),
            static_cast<double>(nv12_req_) / elaps_s, avg_hw, nv12_hw_max_, avg_gpu,
            nv12_gpu_max_, out_w, out_h);
        nv12_req_ = 0;
        nv12_hw_ms_ = nv12_gpu_ms_ = 0.0;
        nv12_hw_max_ = nv12_gpu_max_ = 0.0;
    }
    return frame;
}

namespace {
double media_fps_of(const canvas::core::Project& project, const canvas::core::Clip& clip) {
    const auto it = std::find_if(project.media.begin(), project.media.end(),
                                 [&](const canvas::core::MediaEntry& m) { return m.id == clip.media; });
    return (it != project.media.end() && it->fps > 0.0) ? it->fps : 0.0;
}

// Time-based clip mapping: a seq-frame offset advances the source by the
// media/sequence fps ratio, so 60fps footage on a 30fps timeline strides two
// source frames per timeline frame (the clip plays at its intended speed)
// instead of halving the content. 1:1 whenever the rates match. Speed Change
// (clip_rate) multiplies the offset for a whole-clip retime, keeping playback
// and export on the same law. A clip's src_in/src_out are indices into the
// SOURCE's own frame rate.
int64_t seq_to_src_frame(const canvas::core::Project& project, const canvas::core::Clip& clip,
                         int64_t seq_frame) {
    const double mf = media_fps_of(project, clip);
    const double sf = project.sequence.fps;
    if (mf <= 0.0 || sf <= 0.0) {
        return clip.src_in +
               canvas::core::cliprate::scaled_frame_offset(clip, seq_frame - clip.tl_in);
    }
    return clip.src_in + static_cast<int64_t>(std::llround(
                             static_cast<double>(
                                 canvas::core::cliprate::scaled_frame_offset(
                                     clip, seq_frame - clip.tl_in)) *
                             mf / sf));
}

// Maps a core transition kind to the renderable viewer mode. Audio-only
// transitions (constant gain/power/exponential) carry no image and map to None
// here — they only drive audio mixing.
canvas::core::TransitionRenderMode to_render_mode(const canvas::core::TransitionType t) {
    using TT = canvas::core::TransitionType;
    using RM = canvas::core::TransitionRenderMode;
    switch (t) {
        case TT::CrossDissolve: return RM::CrossDissolve;
        case TT::DipToBlack:    return RM::DipToBlack;
        case TT::FadeOut:       return RM::FadeOut;
        case TT::FadeIn:        return RM::FadeIn;
        case TT::WipeLeft:      return RM::WipeLeft;
        case TT::WipeRight:     return RM::WipeRight;
        case TT::WipeUp:        return RM::WipeUp;
        case TT::WipeDown:      return RM::WipeDown;
        default:                return RM::None;
    }
}

// Copies A's visual transform onto a RenderFrame so the viewport applies it.
void apply_clip_visual(canvas::core::RenderFrame& out,
                       const canvas::core::Clip& a) {
    out.scale_x = a.scale_x;
    out.scale_y = a.scale_y;
    out.pos_x = a.pos_x;
    out.pos_y = a.pos_y;
    out.rotation_deg = a.rotation_deg;
    out.anchor_dx = a.anchor_dx;
    out.anchor_dy = a.anchor_dy;
    out.flip_h = a.flip_h;
    out.flip_v = a.flip_v;
}

// Shared NV12 staging: resize the borrowed hardware frame on the GPU and
// download the Y/UV planes the viewer's NV12 shader uploads. Same reduce rule
// as the RGBA path (longest edge capped at max_dim, 0 = native, even dims) —
// the viewer letterboxes the quad, so the composite needs no bars (dst == full
// canvas). Called by the slot decode path (decode_nv12_slot) AND the off-thread
// transition bake (run_transition_bake), so the two paths are pixel-identical.
// The returned frame carries the source file's resolved color spec (matrix +
// probe-reconciled range) so the shaders decode the raw planes correctly.
canvas::core::Nv12FramePtr host_nv12_from_hw(const AVFrame* hw, std::int64_t src_frame,
                                             const canvas::core::gpu::ColorSpec& spec,
                                             int max_dim) {
    int out_w = hw->width;
    int out_h = hw->height;
    if (max_dim > 0 && (out_w > max_dim || out_h > max_dim)) {
        const double scale = static_cast<double>(max_dim) / std::max(out_w, out_h);
        out_w = std::max(2, static_cast<int>(std::llround(out_w * scale)) & ~1);
        out_h = std::max(2, static_cast<int>(std::llround(out_h * scale)) & ~1);
    } else {
        out_w &= ~1;
        out_h &= ~1;
    }
    out_w = std::max(2, out_w);
    out_h = std::max(2, out_h);

    auto frame = std::make_shared<canvas::core::Nv12Frame>();
    frame->frame_number = src_frame;
    frame->width = out_w;
    frame->height = out_h;
    frame->y_pitch = static_cast<std::size_t>(out_w);
    frame->uv_pitch = static_cast<std::size_t>(out_w);
    frame->matrix = spec.matrix;
    frame->range = spec.range;
    // Staging is a plain per-frame cudaMalloc + resize + memcpy. Transient
    // cudaMalloc flakiness at ~30 allocs/sec surfaced as 1/sec host-nv12-staging
    // nulls that dropped the playhead to the multi-hundred-ms CPU GOP re-walk;
    // retry once, and log attempt-1 with the captured CUDA error + frame layout
    // so the failure is attributable (and known recoverable) by the next run.
    for (int attempt = 0; attempt < 2; ++attempt) {
        if (canvas::core::gpu::convert_nv12_resize_to_host(
                reinterpret_cast<const uint8_t*>(hw->data[0]),
                reinterpret_cast<const uint8_t*>(hw->data[1]),
                hw->width, hw->height,
                static_cast<std::size_t>(hw->linesize[0]),
                static_cast<std::size_t>(hw->linesize[1]),
                out_w, out_h, out_w, out_h, 0, 0, &frame->y, &frame->uv))
            return frame;
        if (attempt == 0)
            ::canvas::core::log::log_warning(
                "[dec] NV12-STAGING-FAIL src=%lld dims=%dx%d host=%dx%d "
                "ls=%d/%d cuda_err=%s",
                static_cast<long long>(src_frame), hw->width, hw->height, out_w, out_h,
                hw->linesize[0], hw->linesize[1],
                canvas::core::gpu::cuda_last_error_string());
    }
    return nullptr;
}
}  // namespace

const canvas::core::Clip* TimelineDecoder::top_video_clip_at(const canvas::core::Project& project,
                                                         std::int64_t seq_frame) const {
    if (seq_frame < 0) return nullptr;
    const canvas::core::Sequence& seq = project.sequence;
    for (std::size_t i = seq.video_tracks.size(); i-- > 0;) {
        const auto& track = seq.video_tracks[i];
        if (track.locked) continue;
        if (const canvas::core::Clip* clip = track.clip_at(seq_frame)) return clip;
    }
    return nullptr;
}

std::optional<TimelineDecoder::TransitionBakeCandidate>
TimelineDecoder::next_transition_bake_candidate(const canvas::core::Project& project,
                                                std::int64_t seq_frame) const {
    TransitionBakeJob job;
    if (!transition_bake_candidate(project, seq_frame, &job)) return std::nullopt;
    return TransitionBakeCandidate{job.win_start, job.win_end};
}

// Nearest qualifying same-media OUT-transition window at-or-ahead of `seq_frame`
// that the off-thread pre-render would cover. Mirrors the live path's B search
// (incoming clip exactly at A's cut on any unlocked video track) and requires
// the two media ids to match — the far-GOP double-walk only exists for a SAME
// file — and A to still be the topmost picture at the window head (a higher
// track covering the head would take precedence at present time and waste the
// bake). Pure timeline scan: no I/O, no thread.
bool TimelineDecoder::transition_bake_candidate(const canvas::core::Project& project,
                                                std::int64_t seq_frame,
                                                TransitionBakeJob* out) const {
    const canvas::core::Sequence& seq = project.sequence;
    const canvas::core::Clip* a_win = nullptr;
    const canvas::core::Clip* b_win = nullptr;
    for (const auto& track : seq.video_tracks) {
        if (track.locked) continue;
        for (const auto& a : track.clips) {
            if (!a.enabled) continue;
            if (!a.has_transition_out() ||
                canvas::core::is_audio_transition(a.transition_out))
                continue;
            const std::int64_t dur_out = a.transition_out_duration;
            if (dur_out <= 0 || dur_out > kTransitionBakeMaxFrames) continue;
            const std::int64_t win_start = a.tl_out - dur_out;
            if (win_start < a.tl_in) continue;  // window must fit inside the clip
            const std::int64_t lead = win_start - seq_frame;
            if (lead < 0 || lead > kTransitionBakeLead) continue;
            const canvas::core::Clip* b = nullptr;
            for (const auto& t2 : seq.video_tracks) {
                if (t2.locked) continue;
                for (const auto& cc : t2.clips) {
                    if (cc.tl_in == a.tl_out && cc.id != a.id) { b = &cc; break; }
                }
                if (b) break;
            }
            if (!b || !b->enabled || b->media != a.media) continue;
            if (top_video_clip_at(project, win_start) != &a) continue;
            if (!a_win || win_start < a_win->tl_out - a_win->transition_out_duration) {
                a_win = &a;
                b_win = b;
            }
        }
    }
    if (!a_win || !b_win) return false;

    auto entry_for = [&](canvas::core::MediaId id) -> const canvas::core::MediaEntry* {
        const auto it = std::find_if(project.media.begin(), project.media.end(),
                                     [&](const canvas::core::MediaEntry& m) { return m.id == id; });
        return it != project.media.end() ? &*it : nullptr;
    };
    const canvas::core::MediaEntry* ae = entry_for(a_win->media);
    const canvas::core::MediaEntry* be = entry_for(b_win->media);
    if (!ae || !be) return false;

    out->a_entry = *ae;
    out->b_entry = *be;
    out->seq_fps = project.sequence.fps;
    out->a = *a_win;
    out->b = *b_win;
    out->win_start = a_win->tl_out - a_win->transition_out_duration;
    out->win_end = a_win->tl_out;
    return true;
}

void TimelineDecoder::maybe_start_transition_bake(const canvas::core::Project& project,
                                                  std::int64_t seq_frame) {
    if (!canvas::core::gpu::cuda_available() || hw_.device_name() != "cuda") return;
    TransitionBakeJob job;
    if (!transition_bake_candidate(project, seq_frame, &job)) return;
    const std::int64_t lead = job.win_start - seq_frame;
    const std::int64_t win_start = job.win_start;
    const std::int64_t win_end = job.win_end;
    const canvas::core::MediaId media = job.a.media;
    std::lock_guard<std::mutex> lk(bake_mutex_);
    if (bake_stop_ || bake_inflight_) return;
    // Already serving this window from a finished bake: don't re-kick.
    if (bake_result_ && seq_frame < bake_result_->win_end) return;
    bake_job_ = std::make_unique<TransitionBakeJob>(std::move(job));
    bake_inflight_ = true;
    if (!bake_thread_.joinable())
        bake_thread_ = std::thread(&TimelineDecoder::transition_bake_thread, this);
    bake_cv_.notify_all();
    ::canvas::core::log::log_warning(
        "[trans-bake] kick media=%d win=[%lld,%lld) lead=%lld seq=%lld",
        static_cast<int>(media), static_cast<long long>(win_start),
        static_cast<long long>(win_end), static_cast<long long>(lead),
        static_cast<long long>(seq_frame));
}

void TimelineDecoder::adopt_or_clear_transition_bake(std::int64_t seq_frame) {
    std::lock_guard<std::mutex> lk(bake_mutex_);
    if (!bake_result_ || seq_frame < bake_result_->win_end) return;
    const canvas::core::MediaId b_media = bake_result_->b_media;
    if (bake_result_->parked_b && bake_result_->parked_b->is_open()) {
        const auto it = slots_.find(b_media);
        if (it != slots_.end() && it->second->loaded) {
            it->second->decoder = std::move(*bake_result_->parked_b);
            // The worker's B slot for this media is now redundant (the adopted
            // main slot is already parked at B's head); drop it so a future cut
            // on the same file reopens fresh instead of pinning two sessions.
            b_slots_.erase(b_media);
            ::canvas::core::log::log_warning(
                "[trans-bake] adopt parked-B media=%d seq=%lld served=%lld",
                static_cast<int>(b_media), static_cast<long long>(seq_frame),
                static_cast<long long>(bake_result_->served));
        } else {
            ::canvas::core::log::log_warning(
                "[trans-bake] adopt-DROPPED media=%d (main slot gone) seq=%lld",
                static_cast<int>(b_media), static_cast<long long>(seq_frame));
        }
    }
    bake_result_.reset();
}

void TimelineDecoder::transition_bake_thread() {
    for (;;) {
        std::unique_ptr<TransitionBakeJob> job;
        {
            std::unique_lock<std::mutex> lk(bake_mutex_);
            bake_cv_.wait(lk, [this] { return bake_stop_ || bake_job_ != nullptr; });
            if (bake_stop_) return;
            job = std::move(bake_job_);
        }
        run_transition_bake(*job);
        {
            std::lock_guard<std::mutex> lk(bake_mutex_);
            bake_inflight_ = false;
        }
    }
}

// Walks the whole bake window with TWO dedicated decoder sessions (A's tail +
// B's pre-roll), converting each frame to the in-memory NV12 planes the viewer
// crossfades — the exact decode+composite work the playback thread would have
// done live, minus the multi-second far keyframe walks (B's open + the main
// slot's post-cut jump), done here AHEAD of the playhead. The B session, parked
// at B's head when the walk ends, is handed to the worker as `parked_b`. Never
// blocks playback: a completed result is simply matched by win bounds in frame().
void TimelineDecoder::run_transition_bake(const TransitionBakeJob& job) {
    const auto t0 = std::chrono::steady_clock::now();
    std::string error;
    canvas::core::VideoDecoder decA, decB;
    if (!decA.open(job.a_entry.path, &error, hw_.device_ctx()) ||
        !decB.open(job.b_entry.path, &error, hw_.device_ctx())) {
        ::canvas::core::log::log_warning(
            "[trans-bake] open-FAILED win=[%lld,%lld) err=%s",
            static_cast<long long>(job.win_start), static_cast<long long>(job.win_end),
            error.c_str());
        return;
    }
    if (!decA.is_hardware() || !decB.is_hardware() || hw_.device_name() != "cuda") {
        ::canvas::core::log::log_warning(
            "[trans-bake] no-hw-skip win=[%lld,%lld)",
            static_cast<long long>(job.win_start), static_cast<long long>(job.win_end));
        return;
    }
    if (!decA.has_iframe_index()) decA.build_iframe_index();
    if (!decB.has_iframe_index()) decB.build_iframe_index();

    const double bsf = job.seq_fps;
    const double amf = (job.a_entry.fps > 0.0) ? job.a_entry.fps : bsf;
    const double bmf = (job.b_entry.fps > 0.0) ? job.b_entry.fps : bsf;
    const double bratio = (bmf > 0.0 && bsf > 0.0) ? bsf / bmf : 1.0;
    const std::int64_t dur_out = job.win_end - job.win_start;
    const std::int64_t n = job.win_end - job.win_start;

    // Decode strategy mirrors the worker's prepared-playback branch
    // (decode_nv12_slot): anchor each session ONCE on the owning keyframe, then
    // ride decode_to_hw's cheap sequential walk for the rest of the window.
    // Calling decode_to_hw_indexed for EVERY frame would container-seek +
    // codec-flush per frame (the seek resets the walk position), re-climbing a
    // whole GOP per frame on both sessions — measured: a 14-frame bake ~2.9s,
    // which never beat the 0.47s window and so never engaged the serve path.
    std::vector<std::pair<canvas::core::Nv12FramePtr, canvas::core::Nv12FramePtr>> planes;
    planes.reserve(static_cast<std::size_t>(n));
    bool failed = false;
    bool a_indexed = true;
    bool b_indexed = true;
    canvas::core::Nv12FramePtr prev_pb;  // reused for rate-rounded B repeats
    std::int64_t prev_b_src = -1;
    for (std::int64_t i = 0; i < n; ++i) {
        {
            std::lock_guard<std::mutex> lk(bake_mutex_);
            if (bake_stop_) {
                failed = true;
                break;
            }
        }
        const std::int64_t seq_frame = job.win_start + i;
        const std::int64_t a_src = job.a.src_in +
            static_cast<std::int64_t>(std::llround(
                static_cast<double>(seq_frame - job.a.tl_in) * amf / bsf));
        std::int64_t b_seq = job.b.tl_in + static_cast<std::int64_t>(std::llround(
            (static_cast<double>(seq_frame - job.win_start) - dur_out) * bratio));
        if (b_seq < 0) b_seq = 0;
        const AVFrame* ha = a_indexed ? decA.decode_to_hw_indexed(a_src)
                                      : decA.decode_to_hw(a_src);
        a_indexed = false;
        if (!ha || !ha->data[0]) {
            failed = true;
            break;
        }
        auto pa = host_nv12_from_hw(ha, a_src, decA.color_spec(), 0);
        if (!pa) {
            failed = true;
            break;
        }
        // B's pre-roll can land on the SAME source frame for consecutive window
        // frames (the 2:1 rate rounds tl offsets onto one media frame). The live
        // path serves that repeat from its retain-hit cache; here a sequential
        // walk cannot step backward, so reuse the previously baked B plane —
        // pixel-identical to what the live retain-hit would render.
        canvas::core::Nv12FramePtr pb;
        if (b_seq == prev_b_src && prev_pb) {
            pb = prev_pb;
        } else {
            const AVFrame* hb = b_indexed ? decB.decode_to_hw_indexed(b_seq)
                                          : decB.decode_to_hw(b_seq);
            b_indexed = false;
            if (!hb || !hb->data[0]) {
                failed = true;
                break;
            }
            pb = host_nv12_from_hw(hb, b_seq, decB.color_spec(), 0);
            if (!pb) {
                failed = true;
                break;
            }
            prev_pb = pb;
            prev_b_src = b_seq;
        }
        planes.emplace_back(std::move(pa), std::move(pb));
    }
    if (failed) {
        ::canvas::core::log::log_warning(
            "[trans-bake] %s win=[%lld,%lld) planes=%zu",
            bake_stop_ ? "ABORTED" : "FAILED",
            static_cast<long long>(job.win_start), static_cast<long long>(job.win_end),
            planes.size());
        return;
    }

    // Park the B session AT B's clip head (the source of the first post-window
    // frame). On a distant-source same-media cut the window's last pre-roll frame
    // sits FAR before src_in (this project: 1678 vs 3448); a parked decoder left
    // there forces the worker's very next frame to re-walk the whole GOP
    // (~200ms) — the visible 1-frame stutter at the cut. Positioning on src_in
    // now, off-thread and absorbed by the bake's lead, makes that first post-cut
    // frame a decode_to_hw_indexed retain-hit, so the boundary is a free
    // sequential continue. The indexed entry preserves an incumbent retain when
    // src_in was already the last walked source (no over-walk to src_in+1).
    if (!bake_stop_) decB.decode_to_hw_indexed(job.b.src_in);

    // Captured under no lock (this thread alone touches the B session now) so the
    // completion log below is safe after the result is published.
    const std::int64_t parked_at = decB.current_frame();
    {
        std::lock_guard<std::mutex> lk(bake_mutex_);
        if (bake_stop_) return;
        auto res = std::make_unique<BakedTransition>();
        res->a_id = job.a.id;
        res->b_media = job.b.media;
        res->win_start = job.win_start;
        res->win_end = job.win_end;
        res->planes = std::move(planes);
        res->parked_b = std::make_unique<canvas::core::VideoDecoder>(std::move(decB));
        bake_result_ = std::move(res);
    }
    const double secs =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    ::canvas::core::log::log_warning(
        "[trans-bake] DONE win=[%lld,%lld) frames=%lld parkedB@%lld (%.2fs)",
        static_cast<long long>(job.win_start), static_cast<long long>(job.win_end),
        static_cast<long long>(n), static_cast<long long>(parked_at), secs);
}

canvas::core::RenderFramePtr TimelineDecoder::frame(const canvas::core::Project& project,
                                                std::int64_t seq_frame) {
    DecodeOriginGuard origin_frame(1);
    // Off-thread transition pre-render lifecycle (all no-ops outside a bake):
    // past a baked window, adopt its parked-B decoder into the main slot so the
    // post-cut boundary is a sequential continue; then look ahead and kick the
    // next bake-eligible window before the playhead reaches it.
    adopt_or_clear_transition_bake(seq_frame);
    maybe_start_transition_bake(project, seq_frame);

    auto out = std::make_shared<canvas::core::RenderFrame>();

    const canvas::core::Clip* a = top_video_clip_at(project, seq_frame);
    if (a) apply_clip_visual(*out, *a);
    if (!a) return out;

    // Graded clips ride the SAME GPU/NV12 fast path as grade-free ones: the
    // grade is baked into a 3D LUT once per grade change and attached to the
    // RenderFrame (out->grade), which the viewer samples in its NV12 shader —
    // no full-res CPU grade eval on the playback path (Phase LUT live graded
    // preview). The CPU RGBA path applies that same LUT, so preview == export
    // by construction.
    const bool grade_a = a->has_grade();

    // Is `seq_frame` inside the transition window owned by A's OUT boundary?
    const int64_t dur_out = a->transition_out_duration;
    const int64_t tr_out_start = a->tl_out - dur_out;
    const bool in_out_trans = a->has_transition_out() &&
                              !canvas::core::is_audio_transition(a->transition_out) &&
                              seq_frame >= tr_out_start && seq_frame < a->tl_out;

    // Single-clip fade at A's IN (leading) boundary: over the first
    // `transition_in_duration` frames the clip fades in from black; no
    // preceding clip required. Independent of any OUT transition.
    const int64_t dur_in = a->transition_in_duration;
    const bool in_in_trans = a->has_transition_in() &&
                             !canvas::core::is_audio_transition(a->transition_in) &&
                             seq_frame >= a->tl_in && seq_frame < a->tl_in + dur_in;

    if (in_in_trans || in_out_trans) {
        // [trans-bake] Serve-from-cache: a completed off-thread pre-render covers
        // this OUT window — hand the baked A/B NV12 planes to the viewer's
        // crossfade shader with the SAME mode/progress/grades the live path would
        // attach, so the playback thread does ZERO decoding across the dissolve
        // (the two far keyframe walks the bake absorbed are both gone). Re-attach
        // grade LUTs from the CURRENT project at present-time — the bake only
        // captures decode, so a grade change mid-window still displays correctly.
        {
            bool serve = false;
            bool first_serve = false;
            {
                std::lock_guard<std::mutex> lk(bake_mutex_);
                BakedTransition* bt = bake_result_.get();
                if (bt && in_out_trans && bt->a_id == a->id &&
                    bt->win_start == tr_out_start && bt->win_end == a->tl_out &&
                    seq_frame >= bt->win_start && seq_frame < bt->win_end) {
                    const std::size_t idx =
                        static_cast<std::size_t>(seq_frame - bt->win_start);
                    if (idx < bt->planes.size() && bt->planes[idx].first) {
                        first_serve = (bt->served++ == 0);
                        out->nv12 = bt->planes[idx].first;
                        out->b_nv12 = bt->planes[idx].second;
                        serve = true;
                    }
                }
            }
            if (serve) {
                if (first_serve)
                    ::canvas::core::log::log_warning(
                        "[trans-bake] serve win=[%lld,%lld) seq=%lld idx=%zu planes=%zu",
                        static_cast<long long>(tr_out_start),
                        static_cast<long long>(a->tl_out), static_cast<long long>(seq_frame),
                        static_cast<std::size_t>(seq_frame - tr_out_start),
                        bake_result_ ? bake_result_->planes.size() : 0u);
                out->grade = grade_a
                                 ? grade_lut_for(*a)
                                 : canvas::core::grade_graph::GradeLutPtr{};
                // Re-resolve the incoming clip so grade_b keys on THIS project's
                // Clip (grade_lut_cache_ is keyed by clip id + grade change_seq,
                // so the id/seq pair is what must match the owner project) —
                // never a bake copy. b_nv12 being null means the bake had no
                // incoming clip (fade-to-black fallback), matching the live no-b
                // branch.
                if (out->b_nv12) {
                    const canvas::core::Sequence& seq = project.sequence;
                    const canvas::core::Clip* b = nullptr;
                    for (const auto& track : seq.video_tracks) {
                        if (track.locked) continue;
                        for (const auto& cc : track.clips)
                            if (cc.tl_in == a->tl_out) { b = &cc; break; }
                        if (b) break;
                    }
                    out->grade_b = (b && b != a && b->has_grade())
                                       ? grade_lut_for(*b)
                                       : canvas::core::grade_graph::GradeLutPtr{};
                }
                out->mode = to_render_mode(a->transition_out);
                if (dur_out > 0)
                    out->progress = static_cast<float>(seq_frame - tr_out_start) /
                                    static_cast<float>(dur_out);
                if (!out->b_nv12) out->fade_to_black = true;
                return out;
            }
        }
        // GPU transition path: try to deliver A (and B, once visible mid-window)
        // as hardware NV12 planes so a cut/cross-fade stays on the GPU fast path
        // instead of the two full-res CPU RGBA decodes that collapsed the
        // transition window to ~1.5 fps at 2K60. Falls through to the RGBA path
        // below when a required plane can't be hardware-decoded.
        const auto nvA = decode_nv12(project, *a, seq_frame, 0);
        if (nvA) {
            // Single-clip IN fade needs only A: the viewer ramps A itself against
            // black in the shader.
            if (in_in_trans && !in_out_trans) {
                out->nv12 = std::move(nvA);
                out->grade = grade_a ? grade_lut_for(*a) : canvas::core::grade_graph::GradeLutPtr{};
                out->mode = to_render_mode(a->transition_in);
                if (dur_in > 0)
                    out->progress = static_cast<float>(seq_frame - a->tl_in) /
                                    static_cast<float>(dur_in);
                out->fade_from_black = true;
                return out;
            }

            // OUT transition: the incoming clip B (sitting exactly at the cut on
            // the same track) plays BEHIND A. Advance B through its pre-roll
            // handle (media frames before its timeline IN) so the dissolve
            // reveals live footage instead of a frozen first frame; clamp to
            // source 0 when the head was trimmed tight against the media start.
            const canvas::core::Sequence& seq = project.sequence;
            const canvas::core::Clip* b = nullptr;
            for (const auto& track : seq.video_tracks) {
                if (track.locked) continue;
                for (const auto& cc : track.clips) {
                    if (cc.tl_in == a->tl_out) { b = &cc; break; }
                }
                if (b) break;
            }
            if (b && b != a) {
                const double bsf = project.sequence.fps;
                const double bmf = media_fps_of(project, *b);
                const double bratio = (bmf > 0.0 && bsf > 0.0) ? bsf / bmf : 1.0;
                int64_t b_seq = b->tl_in + static_cast<int64_t>(std::llround(
                    (static_cast<double>(seq_frame - tr_out_start) - dur_out) * bratio));
                if (b_seq < 0) b_seq = 0;
                auto nvB = [&]() -> canvas::core::Nv12FramePtr {
                    // Same clip media => the adjacent clips share ONE hardware
                    // decoder slot. Reuse A's planes ONLY when B's pre-roll
                    // decodes to the exact media frame A already decoded — a true
                    // 1:1 seamless blade (source ranges contiguous AND rates
                    // matching, which is what maps b_seq back onto A's frame).
                    // Crossfading identical frames is then the seamless-cut the
                    // dissolve intends, and matches what the RGBA path rendered
                    // without re-seeking the shared CUDA session (which would
                    // re-walk up to a full keyframe GOP just to re-decode a
                    // picture we already hold).
                    //
                    // When the pre-roll lands on DIFFERENT footage (trimmed or
                    // retimed cut, gaps in the source, cross-rate project like
                    // 60fps media on a 30fps timeline), aliasing A here would
                    // freeze the dissolve on A for the whole window — mix(A, A,
                    // t) is A for every t. Decode B real footage instead.
                    //
                    // SAME media: A and B share source file but need TWO decode
                    // positions (A's tail + B's pre-roll). A dedicated B slot
                    // walks the pre-roll forward independently of A's session;
                    // decoding B on A's slot would re-walk a whole GOP each
                    // frame (the ~10fps stall / 4.4s A/V drift seen in the
                    // first pass). Distinct media needs no B slot — the two
                    // media slots already decode independently.
                    if (b->media == a->media) {
                        if (seq_to_src_frame(project, *b, b_seq) ==
                            seq_to_src_frame(project, *a, seq_frame))
                            return nvA;
                        open_b_slot(project, *b);
                        auto bit = b_slots_.find(b->media);
                        if (bit != b_slots_.end() && bit->second->loaded)
                            return decode_nv12_slot(bit->second.get(), project, *b,
                                                    b_seq, 0);
                        g_last_nv12_null_reason = "b-slot-not-open";
                        return {};
                    }
                    return decode_nv12(project, *b, b_seq, 0);
                }();
                if (nvB) {
                    out->nv12 = std::move(nvA);
                    out->b_nv12 = std::move(nvB);
                    out->grade = grade_a
                                     ? grade_lut_for(*a)
                                     : canvas::core::grade_graph::GradeLutPtr{};
                    out->grade_b = b->has_grade()
                                       ? grade_lut_for(*b)
                                       : canvas::core::grade_graph::GradeLutPtr{};
                    out->mode = to_render_mode(a->transition_out);
                    if (dur_out > 0)
                        out->progress = static_cast<float>(seq_frame - tr_out_start) /
                                        static_cast<float>(dur_out);
                    return out;
                }
                // B couldn't be hardware-decoded: fall through and render the
                // whole transition on the CPU RGBA path below.
            } else {
                // No incoming clip at the cut (e.g. the last clip on the track):
                // fade A itself out to black over the transition window.
                out->nv12 = std::move(nvA);
                out->grade = grade_a ? grade_lut_for(*a) : canvas::core::grade_graph::GradeLutPtr{};
                out->mode = to_render_mode(a->transition_out);
                if (dur_out > 0) {
                    out->progress = static_cast<float>(seq_frame - tr_out_start) /
                                    static_cast<float>(dur_out);
                }
                out->fade_to_black = true;
                return out;
            }
        }
        // A-side hardware decode unavailable at this position: RGBA path below.
    } else {
        // GPU fast path: HW decode + CUDA composite straight into a small NV12
        // the viewer uploads as Y/UV textures (no full-res CPU RGBA). Falls back
        // to the RGBA path below when unavailable (software decode, non-CUDA
        // device, backward scrub where decode_to_hw can't rewind). Graded clips
        // ride this path too: the LUT rides on the RenderFrame and the viewer's
        // NV12 shader applies it.
        const auto nv12 = decode_nv12(project, *a, seq_frame, 0);
        if (nv12) {
            out->nv12 = std::move(nv12);
            out->grade = grade_a ? grade_lut_for(*a) : canvas::core::grade_graph::GradeLutPtr{};
            return out;
        }
    }

    // CPU decode fallback (NV12 fast path unavailable at this position). The
    // clip's grade is NEVER evaluated here on the CPU: the baked 3D LUT rides
    // on the RenderFrame and the viewer's fragment shader applies it to the
    // RGBA texture exactly like the NV12 shader does — zero CPU LUT work on
    // the playback path.
    const auto fb_t0 = std::chrono::steady_clock::now();
    const char* const fb_why = g_last_nv12_null_reason;
    const auto fb_nv12_ms = g_last_nv12_null_ms;
    out->a = decode(project, *a, seq_frame);
    const double fb_ms = std::chrono::duration<double, std::milli>(
                             std::chrono::steady_clock::now() - fb_t0).count();
    trace_rgba_fallback("A-main", a->media, seq_frame, a->tl_in, fb_why,
                        fb_nv12_ms, fb_ms);
    if (grade_a) out->grade = grade_lut_for(*a);

    if (in_in_trans) {
        out->mode = to_render_mode(a->transition_in);
        if (dur_in > 0)
            out->progress = static_cast<float>(seq_frame - a->tl_in) / static_cast<float>(dur_in);
        out->fade_from_black = true;
    }

    if (in_out_trans) {
        static int transition_log_ = 0;
        if ((transition_log_++ % 30) == 0)
            CANVAS_LOG("transition: playhead active seq_frame %lld clip %llu type %d window [%lld,%lld)",
                   static_cast<long long>(seq_frame), static_cast<unsigned long long>(a->id),
                   static_cast<int>(a->transition_out),
                   static_cast<long long>(tr_out_start), static_cast<long long>(a->tl_out));
        // Incoming clip B sits exactly at the cut (A's tl_out) on the same track.
        const canvas::core::Sequence& seq = project.sequence;
        const canvas::core::Clip* b = nullptr;
        for (const auto& track : seq.video_tracks) {
            if (track.locked) continue;
            for (const auto& cc : track.clips) {
                if (cc.tl_in == a->tl_out) { b = &cc; break; }
            }
            if (b) break;
        }
        if (b && b != a) {
            // The incoming clip plays BEHIND the transition: advance B through its
            // pre-roll handle (media frames before its timeline IN) so the dissolve
            // reveals live footage instead of a frozen first frame, and B keeps
            // playing seamlessly once the cut lands. Clamp to source 0 when the
            // head was trimmed tight against the media start (no handle to show).
            // decode() maps seq->media by the media/sequence ratio, so feed it
            // the inverse-scaled frame: B's seq offset (negative during the
            // window, before its timeline IN) times seq/media.
            const double bsf2 = project.sequence.fps;
            const double bmf2 = media_fps_of(project, *b);
            const double bratio = (bmf2 > 0.0 && bsf2 > 0.0) ? bsf2 / bmf2 : 1.0;
            int64_t b_seq = b->tl_in + static_cast<int64_t>(std::llround(
                (static_cast<double>(seq_frame - tr_out_start) - dur_out) * bratio));
            if (b_seq < 0) b_seq = 0;
            const auto fbB0 = std::chrono::steady_clock::now();
            const char* const fbB_why = g_last_nv12_null_reason;
            const auto fbB_nv12_ms = g_last_nv12_null_ms;
            out->b = decode(project, *b, b_seq);
            const double fbB_ms =
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - fbB0).count();
            trace_rgba_fallback("B-out", b->media, b_seq, b->tl_in, fbB_why,
                                fbB_nv12_ms, fbB_ms);
            if (b->has_grade()) out->grade_b = grade_lut_for(*b);
            if (dur_out > 0)
                out->progress = static_cast<float>(seq_frame - tr_out_start) /
                                static_cast<float>(dur_out);
            out->mode = to_render_mode(a->transition_out);
        } else {
            // No incoming clip at the cut (e.g. the last clip on the track): fade A
            // itself out to black over the transition window.
            if (dur_out > 0) {
                out->progress = static_cast<float>(seq_frame - tr_out_start) /
                                static_cast<float>(dur_out);
            }
            out->fade_to_black = true;
        }
    }

    return out;
}

// Low-res variant used only for scrubbing. Bypasses the full-res cache on read
// but does NOT put the reduced frame back into it, so a preview never displaces
// (or gets returned as) a full-res playback frame.
canvas::core::RenderFramePtr TimelineDecoder::preview(const canvas::core::Project& project,
                                                  std::int64_t seq_frame,
                                                  int max_dim) {
    DecodeOriginGuard origin_preview(2);
    auto out = std::make_shared<canvas::core::RenderFrame>();
    const canvas::core::Clip* a = top_video_clip_at(project, seq_frame);

    // DECISIVE branch trace (always-on): report exactly which early return the
    // scrub preview takes, so a decode that "runs but yields nothing" can't
    // silently evade the [scrub:BAD] fallback path.
    static unsigned trace_ = 0;
    if ((++trace_ & 15u) == 0u)
        ::canvas::core::log::log_warning(
            "[scrub:TRACE] seq=%lld project=%d clip=%d maxdim=%d total=%lld",
            static_cast<long long>(seq_frame), 1,
            top_video_clip_at(project, seq_frame) ? 1 : 0, max_dim,
            static_cast<long long>(project.sequence.duration_frames()));

    if (a) apply_clip_visual(*out, *a);
    if (!a) return out;

    const int64_t dur_out = a->transition_out_duration;
    const int64_t tr_out_start = a->tl_out - dur_out;
    const bool in_out_trans = a->has_transition_out() &&
                              !canvas::core::is_audio_transition(a->transition_out) &&
                              seq_frame >= tr_out_start && seq_frame < a->tl_out;

    const int64_t dur_in = a->transition_in_duration;
    const bool in_in_trans = a->has_transition_in() &&
                             !canvas::core::is_audio_transition(a->transition_in) &&
                             seq_frame >= a->tl_in && seq_frame < a->tl_in + dur_in;

    // Graded clips ride the GPU NV12 fast path like every other clip: the LUT
    // is attached to the RenderFrame and the viewer's NV12 shader applies it
    // (Phase LUT live graded preview). Transition windows too: single-clip
    // fades and cut-dissolves crossfade two hardware NV12 planes in the shader
    // (b_nv12 + mode/progress), so a scrub across a transition stays GPU-speed
    // instead of two full CPU RGBA decodes per frame. The CPU compositor below
    // is the fallback when a required plane can't be hardware-decoded.
    const bool grade_a = a->has_grade();
    bool nv12_had = false, rgba_had = false;
    {
        // GPU fast path with the reduced-cap composite (see frame).
        const auto nvA = decode_nv12(project, *a, seq_frame, max_dim);
        if (nvA) {
            nv12_had = true;
            // Single-clip IN fade needs only A: the viewer ramps A itself against
            // black in the shader.
            if (in_in_trans && !in_out_trans) {
                out->nv12 = std::move(nvA);
                out->grade = grade_a ? grade_lut_for(*a) : canvas::core::grade_graph::GradeLutPtr{};
                out->mode = to_render_mode(a->transition_in);
                if (dur_in > 0)
                    out->progress = static_cast<float>(seq_frame - a->tl_in) /
                                    static_cast<float>(dur_in);
                out->fade_from_black = true;
                return out;
            }
            if (in_out_trans) {
                // The incoming clip B (sitting exactly at the cut on the same
                // track) plays BEHIND A. Advance B through its pre-roll handle so
                // the dissolve reveals live footage instead of a frozen first
                // frame; clamp to source 0 when the head was trimmed tight.
                const canvas::core::Sequence& seq = project.sequence;
                const canvas::core::Clip* b = nullptr;
                for (const auto& track : seq.video_tracks) {
                    if (track.locked) continue;
                    for (const auto& cc : track.clips) {
                        if (cc.tl_in == a->tl_out) { b = &cc; break; }
                    }
                    if (b) break;
                }
                if (b && b != a) {
                    const double bsf = project.sequence.fps;
                    const double bmf = media_fps_of(project, *b);
                    const double bratio = (bmf > 0.0 && bsf > 0.0) ? bsf / bmf : 1.0;
                    int64_t b_seq = b->tl_in + static_cast<int64_t>(std::llround(
                        (static_cast<double>(seq_frame - tr_out_start) - dur_out) * bratio));
                    if (b_seq < 0) b_seq = 0;
                    auto nvB = [&]() -> canvas::core::Nv12FramePtr {
                        // Same clip media => the adjacent clips share ONE media
                        // file but the pre-roll may land on DIFFERENT footage
                        // (trimmed/retimed cut, source gaps, cross-rate project
                        // like 60fps media on a 30fps timeline). Reuse A's
                        // planes ONLY on the true 1:1 seamless blade. Otherwise
                        // decode B for real via its own B slot (a shared-slot
                        // decode would re-walk a GOP every preview frame).
                        if (b->media == a->media) {
                            if (seq_to_src_frame(project, *b, b_seq) ==
                                seq_to_src_frame(project, *a, seq_frame))
                                return nvA;
                            open_b_slot(project, *b);
                            auto bit = b_slots_.find(b->media);
                            if (bit != b_slots_.end() && bit->second->loaded)
                                return decode_nv12_slot(bit->second.get(), project,
                                                        *b, b_seq, max_dim);
                            g_last_nv12_null_reason = "b-slot-not-open";
                            return {};
                        }
                        return decode_nv12(project, *b, b_seq, max_dim);
                    }();
                    if (nvB) {
                        out->nv12 = std::move(nvA);
                        out->b_nv12 = std::move(nvB);
                        out->grade = grade_a
                                         ? grade_lut_for(*a)
                                         : canvas::core::grade_graph::GradeLutPtr{};
                        out->grade_b = b->has_grade()
                                           ? grade_lut_for(*b)
                                           : canvas::core::grade_graph::GradeLutPtr{};
                        out->mode = to_render_mode(a->transition_out);
                        if (dur_out > 0)
                            out->progress = static_cast<float>(seq_frame - tr_out_start) /
                                            static_cast<float>(dur_out);
                        return out;
                    }
                    // B couldn't be hardware-decoded: fall through and render the
                    // whole transition on the CPU RGBA path below.
                } else {
                    // No incoming clip at the cut (e.g. the last clip on the
                    // track): fade A itself out to black over the window.
                    out->nv12 = std::move(nvA);
                    out->grade = grade_a ? grade_lut_for(*a) : canvas::core::grade_graph::GradeLutPtr{};
                    out->mode = to_render_mode(a->transition_out);
                    if (dur_out > 0)
                        out->progress = static_cast<float>(seq_frame - tr_out_start) /
                                        static_cast<float>(dur_out);
                    out->fade_to_black = true;
                    return out;
                }
            } else {
                // Plain frame: NV12 plane + grade LUT.
                out->nv12 = std::move(nvA);
                if (a->has_grade()) {
                    out->grade = grade_lut_for(*a);
                }
                return out;
            }
        }
    }

    const auto fbA0 = std::chrono::steady_clock::now();
    const char* const fbA_why = g_last_nv12_null_reason;
    const auto fbA_nv12_ms = g_last_nv12_null_ms;
    out->a = decode(project, *a, seq_frame, max_dim);
    const double fbA_ms = std::chrono::duration<double, std::milli>(
                              std::chrono::steady_clock::now() - fbA0).count();
    trace_rgba_fallback("A-preview", a->media, seq_frame, a->tl_in, fbA_why,
                        fbA_nv12_ms, fbA_ms);
    if (out->a) {
        rgba_had = true;
        if (grade_a) out->grade = grade_lut_for(*a);
    }

    if (in_in_trans) {
        out->mode = to_render_mode(a->transition_in);
        if (dur_in > 0)
            out->progress = static_cast<float>(seq_frame - a->tl_in) / static_cast<float>(dur_in);
        out->fade_from_black = true;
    }

    if (in_out_trans) {
        static int transition_preview_log_ = 0;
        if ((transition_preview_log_++ % 30) == 0)
            CANVAS_LOG("transition: preview active seq_frame %lld clip %llu type %d max_dim %d",
                   static_cast<long long>(seq_frame), static_cast<unsigned long long>(a->id),
                   static_cast<int>(a->transition_out), max_dim);
        const canvas::core::Sequence& seq = project.sequence;
        const canvas::core::Clip* b = nullptr;
        for (const auto& track : seq.video_tracks) {
            if (track.locked) continue;
            for (const auto& cc : track.clips) {
                if (cc.tl_in == a->tl_out) { b = &cc; break; }
            }
            if (b) break;
        }
        if (b && b != a) {
            const double bsf2 = project.sequence.fps;
            const double bmf2 = media_fps_of(project, *b);
            const double bratio = (bmf2 > 0.0 && bsf2 > 0.0) ? bsf2 / bmf2 : 1.0;
            int64_t b_seq = b->tl_in + static_cast<int64_t>(std::llround(
                (static_cast<double>(seq_frame - tr_out_start) - dur_out) * bratio));
            if (b_seq < 0) b_seq = 0;
            const auto fbB0b = std::chrono::steady_clock::now();
            const char* const fbBb_why = g_last_nv12_null_reason;
            const auto fbBb_nv12_ms = g_last_nv12_null_ms;
            out->b = decode(project, *b, b_seq, max_dim);
            const double fbBb_ms =
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - fbB0b).count();
            trace_rgba_fallback("B-preview", b->media, b_seq, b->tl_in, fbBb_why,
                                fbBb_nv12_ms, fbBb_ms);
            if (out->b && b->has_grade()) out->grade_b = grade_lut_for(*b);
            if (dur_out > 0) out->progress = static_cast<float>(seq_frame - tr_out_start) /
                                             static_cast<float>(dur_out);
            out->mode = to_render_mode(a->transition_out);
        } else {
            if (dur_out > 0) out->progress = static_cast<float>(seq_frame - tr_out_start) /
                                             static_cast<float>(dur_out);
            out->fade_to_black = true;
        }
    }

    // The viewer can only paint a frame that carries pixels. If neither the GPU
    // NV12 plane nor the CPU RGBA came back with content (cold seek, not-yet-
    // loaded slot, failed mid-scrub decode), fall back to a real black frame so
    // the monitor shows black instead of holding a stale picture. The [scrub:BAD]
    // log says exactly which decode path produced nothing, and whether it was a
    // loaded-slot-but-slow decode vs a hard miss.
    if (!out->nv12 && !out->a) {
        ::canvas::core::log::log_warning(
            "[scrub:BAD] seq=%lld media=%d src=%lld gpu_path=%d nv12_ok=%d rgba_ok=%d "
            "slot_loaded=%d hw=%d maxdim=%d",
            static_cast<long long>(seq_frame), a->media,
            static_cast<long long>(a->src_in + (seq_frame - a->tl_in)), nv12_had,
            nv12_had, rgba_had, is_loaded(a->media), is_hardware(a->media), max_dim);
        out->a = make_black_frame(*a, max_dim);
        rgba_had = true;  // packed black pixels; count as paint-able
    }

    return out;
}

double TimelineDecoder::media_rate_at(const canvas::core::Project& project, std::int64_t seq_frame,
                                      double fallback_fps) const {
    if (seq_frame < 0 || seq_frame >= project.sequence.duration_frames()) return fallback_fps;
    const canvas::core::Sequence& seq = project.sequence;
    for (std::size_t i = seq.video_tracks.size(); i-- > 0;) {
        const auto& track = seq.video_tracks[i];
        if (track.locked) continue;
        const canvas::core::Clip* clip = track.clip_at(seq_frame);
        if (!clip) continue;
        const double rate = media_fps_of(project, *clip);
        if (rate > 0.0) return rate;
    }
    return fallback_fps;
}

}  // namespace canvas::gui