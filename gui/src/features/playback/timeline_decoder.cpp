#include "timeline_decoder.hpp"

#include "sync_constants.hpp"

#include "canvas/core/gpu/cuda_convert.hpp"
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

void TimelineDecoder::close() {
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
    preview_cache_.clear();
    preview_lru_.clear();
    grade_lut_cache_.clear();
}

void TimelineDecoder::invalidate(const canvas::core::MediaId media) {
    const size_t before_slots = slots_.size();
    slots_.erase(media);
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

// Phase 6 live graded preview: applies `clip`'s grade to the decoded RGBA
// frame. Returns the frame untouched when the clip owns no grade tree, or when
// the evaluator has no terminal to evaluate, so callers always keep real
// pixels (never a dropped presentation).
//
// Realtime telemetry — ALWAYS-ON, no CANVAS_DEBUG / no cmd needed (stderr +
// ~/studio/canvas_debug.log, flushed per line):
//   * `[grade] engaged`      per clip change: id, source frame, dims, and a
//                            node census (lgg/curves/other counts). The node
//                            census says what a slowdown should be blamed on:
//                            wheels vs curves vs a multi-node tree.
//   * `[grade] SPIKE`        a single apply above ~12ms, warned immediately
//                            (throttled to 1/3s) — a one-frame pathology that a
//                            pure averages window would smooth away.
//   * `[grade] avg/peak`     per-30-frame window summary at INFO level (~1
//                            line/sec at 30fps playback). The dims field tells
//                            preview (≤640 cap) from full-res playback apart.
// Cross-frame totals accrue into take_grade_stats() for the controller's
// per-second `[play]` health line.
canvas::core::VideoFramePtr TimelineDecoder::grade_clip_frame(
    const canvas::core::Clip& clip, canvas::core::VideoFramePtr frame) {
    const canvas::core::grade_graph::GradeLutPtr lut = grade_lut_for(clip);
    if (!frame || !lut) return frame;

    const auto t0 = std::chrono::steady_clock::now();
    const auto graded = canvas::core::grade_graph::apply_grade_lut(*frame, *lut);
    const double ms = std::chrono::duration<double, std::milli>(
                          std::chrono::steady_clock::now() - t0)
                          .count();

    grade_samples_ += 1u;
    grade_ms_sum_ += ms;
    if (ms > grade_ms_max_) grade_ms_max_ = ms;

    static constexpr double kSpikeMs = 12.0;
    static constexpr double kCooldownSec = 3.0;
    static auto last_spike_ = std::chrono::steady_clock::time_point{};
    const auto now = std::chrono::steady_clock::now();
    if (ms > kSpikeMs && (last_spike_ == std::chrono::steady_clock::time_point{} ||
                          now - last_spike_ >= std::chrono::duration<double>(kCooldownSec))) {
        last_spike_ = now;
        ::canvas::core::log::log_warning(
            "[grade] SPIKE clip=%llu src=%lld dims=%dx%d apply_ms=%.2f (CPU LUT)",
            static_cast<unsigned long long>(clip.id),
            static_cast<long long>(frame->frame_number), frame->width, frame->height, ms);
    }

    static constexpr std::uint64_t kReportEvery = 30;
    static std::uint64_t window_frames_ = 0;
    static double window_ms_ = 0.0;
    static double window_peak_ms_ = 0.0;
    window_frames_ += 1u;
    window_ms_ += ms;
    if (ms > window_peak_ms_) window_peak_ms_ = ms;
    if (window_frames_ >= kReportEvery) {
        ::canvas::core::log::log_info(
            "[grade] clip=%llu src=%lld dims=%dx%d apply_ms=%.2f avg_ms=%.2f peak_ms=%.2f "
            "last=%llu frames (CPU LUT)",
            static_cast<unsigned long long>(clip.id),
            static_cast<long long>(frame->frame_number), frame->width, frame->height, ms,
            window_ms_ / static_cast<double>(window_frames_), window_peak_ms_,
            static_cast<unsigned long long>(window_frames_));
        window_frames_ = 0u;
        window_ms_ = 0.0;
        window_peak_ms_ = 0.0;
    }
    return graded ? graded : frame;
}

canvas::core::grade_graph::GradeLutPtr TimelineDecoder::grade_lut_for(
    const canvas::core::Clip& clip) {
    if (!clip.has_grade()) return nullptr;

    const auto it = grade_lut_cache_.find(&clip);
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
    grade_lut_cache_.emplace(&clip, lut);
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
        if (::canvas::core::log::enabled())
            ::canvas::core::log::log_warning(
                "[dec] fullres DECODE media=%d seq=%lld src=%lld ms=%.2f hw=%d dims=%dx%d got=%d",
                clip.media, static_cast<long long>(seq_frame),
                static_cast<long long>(src_frame), dec_ms,
                (int)slot->decoder.is_hardware(), slot->decoder.width(),
                slot->decoder.height(), frame ? frame->width : 0);
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
    (void)project;
    if (clip.media < 0) return nullptr;
    if (!clip.enabled) return nullptr;
    if (!canvas::core::gpu::cuda_available()) return nullptr;

    auto it = slots_.find(clip.media);
    if (it == slots_.end() || !it->second->loaded) return nullptr;
    auto* slot = it->second.get();
    // Build the I-frame index lazily on the GPU path too: decode_to_hw_indexed
    // needs it to anchor a scrub/commit on the owning keyframe. Without it the
    // fallback is a plain container seek + forward decode on every position
    // change (~520ms/scrub observed vs a tens-of-ms one-GOP walk indexed).
    if (!slot->decoder.has_iframe_index()) slot->decoder.build_iframe_index();
    // decode_to_hw serves only the CUDA device and the composite kernel consumes
    // CUDA device pointers, so require hardware decode + a CUDA device.
    if (!slot->decoder.is_hardware() || hw_.device_name() != "cuda") return nullptr;

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
    if (!hw || !hw->data[0] || !hw->data[1]) return nullptr;

    // Same reduce rule as decode_to_frame: cap the longest edge at max_dim
    // (0 = native), preserving aspect. The viewer letterboxes the quad, so the
    // composite needs no bars (dst == full canvas).
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
    // The shaders decode these raw planes with the slot file's resolved color
    // spec — matrix and (probe-reconciled) range — not with assumed 709-limited.
    const canvas::core::gpu::ColorSpec spec = slot->decoder.color_spec();
    frame->matrix = spec.matrix;
    frame->range = spec.range;
    const auto gpu_t0 = std::chrono::steady_clock::now();
    if (!canvas::core::gpu::convert_nv12_resize_to_host(
            reinterpret_cast<const uint8_t*>(hw->data[0]),
            reinterpret_cast<const uint8_t*>(hw->data[1]),
            hw->width, hw->height,
            static_cast<std::size_t>(hw->linesize[0]),
            static_cast<std::size_t>(hw->linesize[1]),
            out_w, out_h, out_w, out_h, 0, 0, &frame->y, &frame->uv))
        return nullptr;
    const double gpu_ms =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - gpu_t0).count();
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
// instead of halving the content. 1:1 whenever the rates match. A clip's
// src_in/src_out are indices into the SOURCE's own frame rate.
int64_t seq_to_src_frame(const canvas::core::Project& project, const canvas::core::Clip& clip,
                         int64_t seq_frame) {
    const double mf = media_fps_of(project, clip);
    const double sf = project.sequence.fps;
    if (mf <= 0.0 || sf <= 0.0) return clip.src_in + (seq_frame - clip.tl_in);
    return clip.src_in + static_cast<int64_t>(std::llround(
                             static_cast<double>(seq_frame - clip.tl_in) * mf / sf));
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

canvas::core::RenderFramePtr TimelineDecoder::frame(const canvas::core::Project& project,
                                                std::int64_t seq_frame) {
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
                if (grade_a && cpu_graded_preview_enabled_)
                    out->a = grade_clip_frame(*a, decode(project, *a, seq_frame,
                                                         kPreviewMaxDim));
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
                    // decoder slot. B's pre-roll handle coincides with A's source
                    // position (B sits at the cut, so b->tl_in == a->tl_out, and
                    // the seq->media map makes b_seq == seq_frame), i.e. B would
                    // decode the exact frame A already has. Decoding it would
                    // re-seek the shared CUDA session backward to a target only A
                    // has reached, re-decoding up to a full 10s keyframe GOP per
                    // transition frame (~250ms on 2K60 — the exact stutter seen in
                    // the logs). Reuse A's planes instead: crossfading identical
                    // frames is the seamless-cut the dissolve intends, and matches
                    // what the RGBA path rendered.
                    if (b->media == a->media) return nvA;
                    // Distinct media => its own decoder slot; the handle advances
                    // forward every frame, so the per-media decode stays sequential.
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
                    if (grade_a && cpu_graded_preview_enabled_)
                        out->a = grade_clip_frame(*a, decode(project, *a, seq_frame,
                                                             kPreviewMaxDim));
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
                if (grade_a && cpu_graded_preview_enabled_)
                    out->a = grade_clip_frame(*a, decode(project, *a, seq_frame,
                                                         kPreviewMaxDim));
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
            // The GPU fast path is the whole story when the Color page is NOT
            // active: display rides the NV12 planes + shader LUT, and there is
            // nothing consuming the small CPU `a`.
            if (grade_a && cpu_graded_preview_enabled_)
                out->a = grade_clip_frame(*a, decode(project, *a, seq_frame,
                                                     kPreviewMaxDim));
            return out;
        }
    }

    out->a = grade_clip_frame(*a, decode(project, *a, seq_frame));

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
            out->b = grade_clip_frame(*b, decode(project, *b, b_seq));
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
                if (grade_a && cpu_graded_preview_enabled_)
                    out->a = grade_clip_frame(*a, decode(project, *a, seq_frame, max_dim));
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
                        // Same clip media => the adjacent clips share ONE hardware
                        // decoder slot, and B would decode the exact frame A already
                        // has (b->tl_in == a->tl_out maps b_seq == seq_frame).
                        // Re-decoding re-seeks the shared CUDA session backward,
                        // re-walking up to a full keyframe GOP per transition frame.
                        // Reuse A's planes: crossfading identical frames is the
                        // seamless-cut the dissolve intends.
                        if (b->media == a->media) return nvA;
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
                        if (grade_a && cpu_graded_preview_enabled_)
                            out->a = grade_clip_frame(*a, decode(project, *a, seq_frame,
                                                                 max_dim));
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
                    if (grade_a && cpu_graded_preview_enabled_)
                        out->a = grade_clip_frame(*a, decode(project, *a, seq_frame,
                                                             max_dim));
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
                    // The GPU path needs no CPU rgba to display; the small graded
                    // feed only exists for the Color-page scopes.
                    if (cpu_graded_preview_enabled_)
                        out->a = grade_clip_frame(*a, decode(project, *a, seq_frame, max_dim));
                }
                return out;
            }
        }
    }

    out->a = grade_clip_frame(*a, decode(project, *a, seq_frame, max_dim));
    if (out->a) rgba_had = true;

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
            out->b = grade_clip_frame(*b, decode(project, *b, b_seq, max_dim));
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