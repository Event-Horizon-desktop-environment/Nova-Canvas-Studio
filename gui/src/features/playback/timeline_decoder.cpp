#include "timeline_decoder.hpp"

#include "sync_constants.hpp"
#include "vaapi_import_state.hpp"

#include "canvas/core/gpu/cuda_convert.hpp"
#include "canvas/core/timeline/clip_rate.hpp"
#include "canvas/core/timeline/title.hpp"
#include "canvas/core/util/color_log.hpp"
#include "canvas/core/util/log.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>

namespace canvas::gui {

namespace {
double media_fps_of(const canvas::core::Project& project, const canvas::core::Clip& clip);
int64_t seq_to_src_frame(const canvas::core::Project& project, const canvas::core::Clip& clip,
                         int64_t seq_frame);
canvas::core::Nv12FramePtr host_nv12_from_hw(const AVFrame* hw, std::int64_t src_frame,
                                             const canvas::core::gpu::ColorSpec& spec,
                                             int max_dim);

thread_local const char* g_last_nv12_null_reason = "never-tried";
thread_local std::int64_t g_last_nv12_null_ms = 0;

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
}

void TimelineDecoder::add_media(const canvas::core::MediaEntry& entry) {
    auto slot = std::make_unique<DecoderSlot>();
    std::string error;
    if (slot->decoder.open(entry.path, &error, hw_.device_ctx(),
                           hw_.device_label().c_str())) slot->loaded = true;
    if (slot->loaded) {
        const bool gpu_tag = slot->decoder.is_hardware() &&
                             slot->decoder.gpu_label()[0];
        ::canvas::core::log::log_warning(
            "[media] open id=%d hw=%s%s%s %s path=%s", entry.id,
            slot->decoder.is_hardware() ? slot->decoder.hardware_name() : "sw",
            gpu_tag ? " gpu=\"" : "", gpu_tag ? slot->decoder.gpu_label() : "",
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
    if (b_slots_.count(clip.media) > 0) return;
    const auto it = std::find_if(project.media.begin(), project.media.end(),
                                 [&](const canvas::core::MediaEntry& m) { return m.id == clip.media; });
    if (it == project.media.end()) return;
    auto slot = std::make_unique<DecoderSlot>();
    std::string error;
    if (slot->decoder.open(it->path, &error, hw_.device_ctx(),
                           hw_.device_label().c_str())) {
        slot->loaded = true;
        const bool gpu_tag = slot->decoder.is_hardware() &&
                             slot->decoder.gpu_label()[0];
        ::canvas::core::log::log_warning(
            "[dec] B-slot open id=%d hw=%s%s%s path=%s", clip.media,
            slot->decoder.is_hardware() ? "yes" : "no",
            gpu_tag ? " gpu=\"" : "", gpu_tag ? slot->decoder.gpu_label() : "",
            it->path.c_str());
    } else {
        ::canvas::core::log::log_warning("[dec] B-slot OPEN-FAILED id=%d err=%s path=%s",
                                         clip.media,
                                         error.empty() ? "unknown" : error.c_str(),
                                         it->path.c_str());
    }
    b_slots_[clip.media] = std::move(slot);
}

void TimelineDecoder::close() {
    {
        std::lock_guard<std::mutex> lk(bake_mutex_);
        bake_stop_ = true;
        bake_cv_.notify_all();
    }
    if (bake_thread_.joinable()) bake_thread_.join();
    {
        std::lock_guard<std::mutex> lk(bake_mutex_);
        bake_stop_ = false;
        bake_job_.reset();
        bake_inflight_ = false;
        bake_result_.reset();
    }

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
        for (std::size_t i = 0; i < g.num_nodes(); ++i) {
            switch (g.node(i).correct_mode) {
                case canvas::core::grade_graph::CorrectMode::kLgg:
                    ++lgg;
                    break;
                case canvas::core::grade_graph::CorrectMode::kCurves:
                    ++curves;
                    break;
                default:
                    ++other;
                    break;
            }
        }
        ::canvas::core::log::log_info(
            "[grade] engaged clip=%llu nodes=%d lgg=%d curves=%d other=%d (3D LUT path)",
            static_cast<unsigned long long>(clip.id), static_cast<int>(g.num_nodes()), lgg, curves,
            other);
    }

    const auto t0 = std::chrono::steady_clock::now();
    {
        using namespace canvas::core::grade_graph;
        using namespace canvas::core::colorsci;
        const auto& g = clip.grade;
        bool any = false;
        for (std::size_t i = 0; i < g.num_nodes(); ++i) {
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
                static_cast<unsigned long long>(clip.id), static_cast<int>(g.num_nodes()));
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

    if (max_dim > 0) {
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
        const auto evict_delta = cs.evictions < last_cache_stats_.evictions
            ? 0 : cs.evictions - last_cache_stats_.evictions;
        const auto miss_delta = cs.misses < last_cache_stats_.misses
            ? 0 : cs.misses - last_cache_stats_.misses;
        last_cache_stats_ = cs;
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
    const std::string& dev = hw_.device_name();
    if (!canvas::core::gpu::cuda_available() && dev != "vaapi") {
        g_last_nv12_null_reason = "no-cuda-no-vaapi";
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
    if (!slot->decoder.has_iframe_index()) slot->decoder.build_iframe_index();
    const std::string& dev = hw_.device_name();
    const bool is_cuda = dev == "cuda";
    const bool is_vaapi = dev == "vaapi";
    if (!slot->decoder.is_hardware() || !(is_cuda || is_vaapi)) {
        g_last_nv12_null_reason = "not-hardware-or-unsupported-device";
        return nullptr;
    }

    if (is_vaapi && !canvas::gui::vaapi_viewer_import_available()) {
        g_last_nv12_null_reason = "vaapi-viewer-import-unavailable";
        return nullptr;
    }

    const int64_t src_frame = seq_to_src_frame(project, clip, seq_frame);
    const AVFrame* hw;
    double hw_ms = 0.0;
    const auto hw_t0 = std::chrono::steady_clock::now();
    if (max_dim > 0) {
        hw = slot->decoder.decode_to_hw_indexed(
            src_frame, ::canvas::core::VideoDecoder::kPreviewMaxOver);
    } else {
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

    const canvas::core::gpu::ColorSpec spec = slot->decoder.color_spec();
    const auto gpu_t0 = std::chrono::steady_clock::now();
    canvas::core::Nv12FramePtr frame;
    if (is_vaapi) {
        auto surf = slot->decoder.vaapi_export_surface(hw, src_frame);
        if (!surf) {
            g_last_nv12_null_reason = "vaapi-export-null";
            g_last_nv12_null_ms = 0;
            return nullptr;
        }
        auto vf = std::make_shared<canvas::core::Nv12Frame>();
        vf->frame_number = src_frame;
        vf->width = surf->width;
        vf->height = surf->height;
        vf->matrix = surf->matrix;
        vf->range = surf->range;
        vf->gpu = std::move(surf);
        frame = std::move(vf);
    } else {
        frame = host_nv12_from_hw(hw, src_frame, spec, max_dim);
        if (!frame) {
            g_last_nv12_null_reason = "host-nv12-staging-null";
            g_last_nv12_null_ms = 0;
            return nullptr;
        }
    }
    const double gpu_ms =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - gpu_t0).count();
    const int out_w = frame->width;
    const int out_h = frame->height;
    static auto nv12_log_at = std::chrono::steady_clock::now();
    static uint64_t nv12_req_ = 0;
    static double nv12_hw_ms_ = 0.0, nv12_gpu_ms_ = 0.0;
    static double nv12_hw_max_ = 0.0, nv12_gpu_max_ = 0.0;
    ++nv12_req_;
    nv12_hw_ms_ += hw_ms;
    nv12_gpu_ms_ += gpu_ms;
    nv12_hw_max_ = std::max(nv12_hw_max_, hw_ms);
    nv12_gpu_max_ = std::max(nv12_gpu_max_, gpu_ms);
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
}

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

const canvas::core::Clip* TimelineDecoder::media_clip_beneath(
    const canvas::core::Project& project, std::int64_t seq_frame) const {
    if (seq_frame < 0) return nullptr;
    const canvas::core::Sequence& seq = project.sequence;
    for (std::size_t i = seq.video_tracks.size(); i-- > 0;) {
        const auto& track = seq.video_tracks[i];
        if (track.locked) continue;
        const canvas::core::Clip* c = track.clip_at(seq_frame);
        if (c && c->media >= 0) return c;
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
            if (win_start < a.tl_in) continue;
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

void TimelineDecoder::run_transition_bake(const TransitionBakeJob& job) {
    const auto t0 = std::chrono::steady_clock::now();
    std::string error;
    canvas::core::VideoDecoder decA, decB;
    if (!decA.open(job.a_entry.path, &error, hw_.device_ctx(),
                   hw_.device_label().c_str()) ||
        !decB.open(job.b_entry.path, &error, hw_.device_ctx(),
                   hw_.device_label().c_str())) {
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

    std::vector<std::pair<canvas::core::Nv12FramePtr, canvas::core::Nv12FramePtr>> planes;
    planes.reserve(static_cast<std::size_t>(n));
    bool failed = false;
    bool a_indexed = true;
    bool b_indexed = true;
    canvas::core::Nv12FramePtr prev_pb;
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

    if (!bake_stop_) decB.decode_to_hw_indexed(job.b.src_in);

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

void TimelineDecoder::attach_title_transition(canvas::core::RenderFrame& out,
                                              const canvas::core::Project& project,
                                              const canvas::core::Clip& a,
                                              std::int64_t seq_frame) {
    const int64_t dur_out = a.transition_out_duration;
    const int64_t tr_out_start = a.tl_out - dur_out;
    const bool in_out_trans = a.has_transition_out() &&
                              !canvas::core::is_audio_transition(a.transition_out) &&
                              seq_frame >= tr_out_start && seq_frame < a.tl_out;
    const int64_t dur_in = a.transition_in_duration;
    const bool in_in_trans = a.has_transition_in() &&
                             !canvas::core::is_audio_transition(a.transition_in) &&
                             seq_frame >= a.tl_in && seq_frame < a.tl_in + dur_in;

    if (in_in_trans) {
        static int title_in_log_ = 0;
        if ((title_in_log_++ % 30) == 0)
            ::canvas::core::log::log_warning(
                "transition: playhead active (title-in) seq_frame %lld clip %llu "
                "type %d window [%lld,%lld)",
                static_cast<long long>(seq_frame),
                static_cast<unsigned long long>(a.id),
                static_cast<int>(a.transition_in),
                static_cast<long long>(a.tl_in),
                static_cast<long long>(a.tl_in + dur_in));
        out.mode = to_render_mode(a.transition_in);
        if (dur_in > 0)
            out.progress = static_cast<float>(seq_frame - a.tl_in) /
                           static_cast<float>(dur_in);
        out.fade_from_black = true;
    }
    if (!in_out_trans) return;

    static int title_out_log_ = 0;
    if ((title_out_log_++ % 30) == 0)
        ::canvas::core::log::log_warning(
            "transition: playhead active (title-out) seq_frame %lld clip %llu "
            "type %d window [%lld,%lld]",
            static_cast<long long>(seq_frame), static_cast<unsigned long long>(a.id),
            static_cast<int>(a.transition_out), static_cast<long long>(tr_out_start),
            static_cast<long long>(a.tl_out));
    const canvas::core::Sequence& seq = project.sequence;
    const canvas::core::Clip* b = nullptr;
    for (const auto& track : seq.video_tracks) {
        if (track.locked) continue;
        for (const auto& cc : track.clips) {
            if (cc.tl_in == a.tl_out) {
                b = &cc;
                break;
            }
        }
        if (b) break;
    }
    if (b && b != &a) {
        const double bsf2 = project.sequence.fps;
        const double bmf2 = media_fps_of(project, *b);
        const double bratio = (bmf2 > 0.0 && bsf2 > 0.0) ? bsf2 / bmf2 : 1.0;
        int64_t b_seq = b->tl_in + static_cast<int64_t>(std::llround(
            (static_cast<double>(seq_frame - tr_out_start) - dur_out) * bratio));
        if (b_seq < 0) b_seq = 0;
        const auto fbB0 = std::chrono::steady_clock::now();
        const char* const fbB_why = g_last_nv12_null_reason;
        const auto fbB_nv12_ms = g_last_nv12_null_ms;
        out.b = decode(project, *b, b_seq);
        const double fbB_ms =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - fbB0).count();
        trace_rgba_fallback("B-title-out", b->media, b_seq, b->tl_in, fbB_why,
                            fbB_nv12_ms, fbB_ms);
        if (b->has_grade()) out.grade_b = grade_lut_for(*b);
        if (dur_out > 0)
            out.progress = static_cast<float>(seq_frame - tr_out_start) /
                           static_cast<float>(dur_out);
        out.mode = to_render_mode(a.transition_out);
    } else {
        if (dur_out > 0)
            out.progress = static_cast<float>(seq_frame - tr_out_start) /
                           static_cast<float>(dur_out);
        out.fade_to_black = true;
    }
}

canvas::core::RenderFramePtr TimelineDecoder::frame(const canvas::core::Project& project,
                                                std::int64_t seq_frame) {
    DecodeOriginGuard origin_frame(1);
    adopt_or_clear_transition_bake(seq_frame);
    maybe_start_transition_bake(project, seq_frame);

    auto out = std::make_shared<canvas::core::RenderFrame>();

    const canvas::core::Clip* a = top_video_clip_at(project, seq_frame);
    if (a) apply_clip_visual(*out, *a);
    if (!a) return out;

    if (a->has_title()) {
        canvas::core::VideoFramePtr source;
        if (a->media >= 0) {
            source = decode(project, *a, seq_frame);
            if (a->has_grade()) out->grade = grade_lut_for(*a);
        } else {
            const canvas::core::Clip* base = media_clip_beneath(project, seq_frame);
            if (base) {
                source = decode(project, *base, seq_frame);
                if (base->has_grade()) out->grade = grade_lut_for(*base);
                apply_clip_visual(*out, *base);
            }
        }
        if (!source) source = make_black_frame(*a, 0);
        if (source) {
            auto writable = std::make_shared<canvas::core::VideoFrame>(*source);
            canvas::core::title::render_clip_title(*a, writable->rgba, writable->width,
                                                   writable->height, writable->stride);
            out->a = std::move(writable);
        }
        attach_title_transition(*out, project, *a, seq_frame);
        return out;
    }

    const bool grade_a = a->has_grade();

    const int64_t dur_out = a->transition_out_duration;
    const int64_t tr_out_start = a->tl_out - dur_out;
    const bool in_out_trans = a->has_transition_out() &&
                              !canvas::core::is_audio_transition(a->transition_out) &&
                              seq_frame >= tr_out_start && seq_frame < a->tl_out;

    const int64_t dur_in = a->transition_in_duration;
    const bool in_in_trans = a->has_transition_in() &&
                             !canvas::core::is_audio_transition(a->transition_in) &&
                             seq_frame >= a->tl_in && seq_frame < a->tl_in + dur_in;

    if (in_in_trans || in_out_trans) {
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
        const auto nvA = decode_nv12(project, *a, seq_frame, 0);
        if (nvA) {
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
            } else {
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
    } else {
        const auto nv12 = decode_nv12(project, *a, seq_frame, 0);
        if (nv12) {
            out->nv12 = std::move(nv12);
            out->grade = grade_a ? grade_lut_for(*a) : canvas::core::grade_graph::GradeLutPtr{};
            return out;
        }
    }

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
            ::canvas::core::log::log_warning("transition: playhead active seq_frame %lld clip %llu type %d window [%lld,%lld)",
                   static_cast<long long>(seq_frame), static_cast<unsigned long long>(a->id),
                   static_cast<int>(a->transition_out),
                   static_cast<long long>(tr_out_start), static_cast<long long>(a->tl_out));
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
            if (dur_out > 0) {
                out->progress = static_cast<float>(seq_frame - tr_out_start) /
                                static_cast<float>(dur_out);
            }
            out->fade_to_black = true;
        }
    }

    return out;
}

canvas::core::RenderFramePtr TimelineDecoder::preview(const canvas::core::Project& project,
                                                  std::int64_t seq_frame,
                                                  int max_dim) {
    DecodeOriginGuard origin_preview(2);
    auto out = std::make_shared<canvas::core::RenderFrame>();
    const canvas::core::Clip* a = top_video_clip_at(project, seq_frame);

    static unsigned trace_ = 0;
    if ((++trace_ & 15u) == 0u)
        ::canvas::core::log::log_warning(
            "[scrub:TRACE] seq=%lld project=%d clip=%d maxdim=%d total=%lld",
            static_cast<long long>(seq_frame), 1,
            top_video_clip_at(project, seq_frame) ? 1 : 0, max_dim,
            static_cast<long long>(project.sequence.duration_frames()));

    if (a) apply_clip_visual(*out, *a);
    if (!a) return out;

    if (a->has_title()) {
        canvas::core::VideoFramePtr source;
        if (a->media >= 0) {
            source = decode(project, *a, seq_frame);
            if (a->has_grade()) out->grade = grade_lut_for(*a);
        } else {
            const canvas::core::Clip* base = media_clip_beneath(project, seq_frame);
            if (base) {
                source = decode(project, *base, seq_frame);
                if (base->has_grade()) out->grade = grade_lut_for(*base);
                apply_clip_visual(*out, *base);
            }
        }
        if (!source) source = make_black_frame(*a, 0);
        if (source) {
            auto writable = std::make_shared<canvas::core::VideoFrame>(*source);
            canvas::core::title::render_clip_title(*a, writable->rgba, writable->width,
                                                   writable->height, writable->stride);
            out->a = std::move(writable);
        }
        attach_title_transition(*out, project, *a, seq_frame);
        return out;
    }

    const int64_t dur_out = a->transition_out_duration;
    const int64_t tr_out_start = a->tl_out - dur_out;
    const bool in_out_trans = a->has_transition_out() &&
                              !canvas::core::is_audio_transition(a->transition_out) &&
                              seq_frame >= tr_out_start && seq_frame < a->tl_out;

    const int64_t dur_in = a->transition_in_duration;
    const bool in_in_trans = a->has_transition_in() &&
                             !canvas::core::is_audio_transition(a->transition_in) &&
                             seq_frame >= a->tl_in && seq_frame < a->tl_in + dur_in;

    const bool grade_a = a->has_grade();
    bool nv12_had = false, rgba_had = false;
    {
        const auto nvA = decode_nv12(project, *a, seq_frame, max_dim);
        if (nvA) {
            nv12_had = true;
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
                } else {
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
            ::canvas::core::log::log_warning("transition: preview active seq_frame %lld clip %llu type %d max_dim %d",
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

    if (!out->nv12 && !out->a) {
        ::canvas::core::log::log_warning(
            "[scrub:BAD] seq=%lld media=%d src=%lld gpu_path=%d nv12_ok=%d rgba_ok=%d "
            "slot_loaded=%d hw=%d maxdim=%d",
            static_cast<long long>(seq_frame), a->media,
            static_cast<long long>(a->src_in + (seq_frame - a->tl_in)), nv12_had,
            nv12_had, rgba_had, is_loaded(a->media), is_hardware(a->media), max_dim);
        out->a = make_black_frame(*a, max_dim);
        rgba_had = true;
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

}
