#include "timeline_decoder.hpp"

#include "sync_constants.hpp"

#include "canvas/core/gpu/cuda_convert.hpp"
#include "canvas/core/util/log.hpp"

#include <algorithm>
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
    slots_.clear();
    preview_cache_.clear();
    preview_lru_.clear();
}

void TimelineDecoder::invalidate(const canvas::core::MediaId media) {
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
            }
        }
        return frame;
    }

    auto frame = slot->cache.get(src_frame);
    if (!frame) {
        frame = slot->decoder.decode_to_frame(src_frame);
        // The decoder labels a frame with its *decoded* PTS-derived number, which can
        // differ from the requested target (a forward-walk holding the first frame
        // at/after the target, or PTS/rate skew). Cache only when the numbers
        // agree: keying the LRU by a wrong number aliases the frame (the playhead
        // would later get frame N when src_frame M was asked for). A miss just
        // re-decodes.
        if (frame && frame->frame_number == src_frame) slot->cache.put(frame);
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
    if (!canvas::core::gpu::convert_nv12_resize_to_host(
            reinterpret_cast<const uint8_t*>(hw->data[0]),
            reinterpret_cast<const uint8_t*>(hw->data[1]),
            hw->width, hw->height,
            static_cast<std::size_t>(hw->linesize[0]),
            static_cast<std::size_t>(hw->linesize[1]),
            out_w, out_h, out_w, out_h, 0, 0, &frame->y, &frame->uv))
        return nullptr;
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

    // A single-clip fade must go through the RGBA path (the viewer alpha-blends
    // `a` against black); the raw GPU NV12 fast path can't express it.
    const bool need_rgba = in_out_trans || in_in_trans;

    if (!need_rgba) {
        // GPU fast path: HW decode + CUDA composite straight into a small NV12
        // the viewer uploads as Y/UV textures (no full-res CPU RGBA). Falls back
        // to the RGBA path below when unavailable (software decode, non-CUDA
        // device, backward scrub where decode_to_hw can't rewind).
        if (auto nv12 = decode_nv12(project, *a, seq_frame, 0)) {
            out->nv12 = std::move(nv12);
            return out;
        }
    }

    out->a = decode(project, *a, seq_frame);

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
            out->b = decode(project, *b, b_seq);
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

    const bool need_rgba = in_out_trans || in_in_trans;

    bool nv12_had = false, rgba_had = false;
    if (!need_rgba) {
        // GPU fast path with the reduced-cap composite (see frame).
        if (auto nv12 = decode_nv12(project, *a, seq_frame, max_dim)) {
            nv12_had = true;
            out->nv12 = std::move(nv12);
            return out;
        }
    }

    out->a = decode(project, *a, seq_frame, max_dim);
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
            out->b = decode(project, *b, b_seq, max_dim);
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
            static_cast<long long>(a->src_in + (seq_frame - a->tl_in)), (!need_rgba),
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