#include "canvas/core/export/renderer.hpp"

#include "canvas/core/media/audio_decoder.hpp"
#include "canvas/core/media/video_decoder.hpp"
#include "canvas/core/timeline/audio_fade.hpp"
#include "canvas/core/timeline/audio_mix.hpp"
#include "canvas/core/util/log.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <vector>

extern "C" {
#include <libavutil/hwcontext.h>
}

namespace canvas::core {

namespace {

// Timeline frame -> source media frame for a clip.
int64_t clip_src_frame(const Clip& clip, int64_t tl_frame) {
    return clip.src_in + (tl_frame - clip.tl_in);
}

double media_fps_for(const Project& project, const Clip& clip) {
    const MediaEntry* m = project.media_by_id(clip.media);
    if (m && m->fps > 0.0) return m->fps;
    return project.sequence.fps;
}

// Returns the first enabled clip on `kind` tracks at `tl_frame`, preferring the
// topmost (last) track, or nullptr.
const Clip* top_clip_at(const Sequence& seq, Track::Kind kind, int64_t tl_frame) {
    const auto& tracks = kind == Track::Kind::Video ? seq.video_tracks : seq.audio_tracks;
    for (std::size_t i = tracks.size(); i-- > 0;) {
        const auto& track = tracks[i];
        if (track.locked) continue;
        const Clip* c = track.clip_at(tl_frame);
        if (c && c->enabled) return c;
    }
    return nullptr;
}

// Adds the interleaved `src` chunk into `out` (a [frames x out_channels]
// buffer) applying the per-frame transition envelope `gains`, the clip's
// linear volume `vol` and the stereo pan balance (gl/gr). Mirrors
// AudioPipeline::apply_mix_gain's fold-down so playback and export sum
// identically (mono/stereo sources and >2ch surround folds).
void mix_audio_chunk(std::vector<float>& out, const std::vector<float>& src, int src_ch,
                     int out_channels, const std::vector<float>& gains, float vol,
                     float gl, float gr) {
    for (std::size_t s = 0; s < src.size(); ++s) {
        const std::size_t frame_idx = s / static_cast<std::size_t>(src_ch);
        const std::size_t src_sc = s % static_cast<std::size_t>(src_ch);
        const float base = gains[frame_idx] * vol;
        if (out_channels == 2) {
            const std::size_t dl = frame_idx * 2u;
            if (src_sc == 0) {
                out[dl] += src[s] * base * gl;
            } else if (src_sc == 1) {
                out[dl + 1u] += src[s] * base * gr;
            } else {
                // Surround fold-down, matching the playback mix.
                const std::size_t dst = (src_sc % 2u == 0u) ? dl : dl + 1u;
                out[dst] += src[s] * base * (src_sc % 2u == 0u ? gl : gr) * 0.5f;
            }
        } else {
            const std::size_t dst_sc =
                std::min<std::size_t>(src_sc, static_cast<std::size_t>(out_channels) - 1);
            out[frame_idx * static_cast<std::size_t>(out_channels) + dst_sc] +=
                src[s] * base;
        }
    }
}

// 2D box blit that composites a decoded source RGBA frame onto a canvas,
// scaling to `dst_w x dst_h` (letterboxed) at `dx,dy`. Alpha is not used; an
// upper video track fully replaces the lower content where it covers.
void blit_rgba(const VideoFrame& src, std::vector<uint8_t>& canvas, int canvas_w,
               int canvas_h, int dst_w, int dst_h, int dx, int dy) {
    if (canvas_w <= 0 || canvas_h <= 0 || dst_w <= 0 || dst_h <= 0) return;
    const std::size_t dst_stride = static_cast<std::size_t>(canvas_w) * 4u;
    for (int y = 0; y < dst_h; ++y) {
        const int cy = dy + y;
        if (cy < 0 || cy >= canvas_h) continue;
        const int sy = std::clamp(static_cast<int>(static_cast<std::size_t>(y) * src.height /
                                                   static_cast<std::size_t>(dst_h)),
                                  0, src.height - 1);
        const std::size_t srow = static_cast<std::size_t>(sy) * src.stride;
        std::size_t drow = static_cast<std::size_t>(cy) * dst_stride;
        for (int x = 0; x < dst_w; ++x) {
            const int cx = dx + x;
            if (cx < 0 || cx >= canvas_w) continue;
            const int sx = std::clamp(static_cast<int>(static_cast<std::size_t>(x) * src.width /
                                                       static_cast<std::size_t>(dst_w)),
                                      0, src.width - 1);
            const std::size_t so = srow + static_cast<std::size_t>(sx) * 4u;
            std::size_t dpo = drow + static_cast<std::size_t>(cx) * 4u;
            canvas[dpo + 0] = src.rgba[so + 0];
            canvas[dpo + 1] = src.rgba[so + 1];
            canvas[dpo + 2] = src.rgba[so + 2];
            canvas[dpo + 3] = 255;
        }
    }
}

inline uint8_t clamp_byte(int v) {
    return static_cast<uint8_t>(v < 0 ? 0 : (v > 255 ? 255 : v));
}

// Blend of a single `base` (canvas) channel-pair with `src` using `mode`, then
// dissolved by `opacity` over `base`: final = blend * opacity + base*(1-opacity).
// The identity (Normal + opacity 1) collapses to `src`, so the caller's fast
// path bypasses this entirely and stays byte-identical to blit_rgba.
inline uint8_t blend_channel(const int mode, const float opacity, const int base,
                             const int src) {
    int blended;
    switch (mode) {
        case 1:  // Add
            blended = base + src;
            break;
        case 2:  // Multiply
            blended = base * src / 255;
            break;
        case 3:  // Screen
            blended = 255 - (255 - base) * (255 - src) / 255;
            break;
        case 4:  // Overlay
            blended = base < 128 ? 2 * base * src / 255
                                 : 255 - 2 * (255 - base) * (255 - src) / 255;
            break;
        default:  // Normal
            blended = src;
            break;
    }
    const float a = opacity;
    const float fb = static_cast<float>(blended) * a + static_cast<float>(base) * (1.0f - a);
    return clamp_byte(static_cast<int>(std::lround(fb)));
}

// Transform + composite blit. The base rect is the fitted letterboxed rect of
// the source (bx,by,size base_w x base_h) whose CENTER is the origin of the
// clip's visual transform. The forward mapping per output pixel `o`:
//
//   final = P + R(rot) * F * S * (o_in_base - P) + (pos_x, pos_y)
//
// where P = base-rect center + (anchor_dx, anchor_dy) is the rotation pivot, S
// scales by (scale_x, scale_y), F mirrors (flip_h/flip_v) about the pivot, and
// R rotates. Nearest-sample from the source. Fast path: when the clip has no
// transform and is opaque/Normal, delegates to blit_rgba for byte-identical
// output. Otherwise each destination pixel inside the bounding box of the
// mapped quad is inverse-mapped and blender-overlaid per clip->blend_mode with
// clip->opacity.
void blit_rgba_transformed(const VideoFrame& src, std::vector<uint8_t>& canvas,
                           int canvas_w, int canvas_h, int base_w, int base_h,
                           int bx, int by, const Clip& clip) {
    if (canvas_w <= 0 || canvas_h <= 0 || base_w <= 0 || base_h <= 0) return;
    if (!clip.has_visual_transform() && !clip.needs_compositing()) {
        blit_rgba(src, canvas, canvas_w, canvas_h, base_w, base_h, bx, by);
        return;
    }
    if (clip.scale_x <= 0.0f || clip.scale_y <= 0.0f) return;

    constexpr double kPi = 3.14159265358979323846;
    const double ang = clip.rotation_deg * kPi / 180.0;
    const double cs = std::cos(ang);
    const double sn = std::sin(ang);
    const double cx = bx + base_w * 0.5;
    const double cy = by + base_h * 0.5;
    const double px = cx + clip.anchor_dx;
    const double py = cy + clip.anchor_dy;
    const double fx = clip.flip_h ? -1.0 : 1.0;
    const double fy = clip.flip_v ? -1.0 : 1.0;
    const double sx_ = clip.scale_x;
    const double sy_ = clip.scale_y;
    const double pxx = clip.pos_x;
    const double pyy = clip.pos_y;
    const int mode = static_cast<int>(clip.blend_mode);

    const double left = bx, top = by, right = bx + base_w, bottom = by + base_h;
    double min_x = left, min_y = top, max_x = right, max_y = bottom;
    {
        const double corners[4][2] = {
            {left - px, top - py}, {right - px, top - py},
            {left - px, bottom - py}, {right - px, bottom - py}};
        for (const auto& c : corners) {
            const double sw = c[0] * fx * sx_;
            const double sh = c[1] * fy * sy_;
            const double ox = px + (sw * cs - sh * sn) + pxx;
            const double oy = py + (sw * sn + sh * cs) + pyy;
            if (ox < min_x) min_x = ox;
            if (ox > max_x) max_x = ox;
            if (oy < min_y) min_y = oy;
            if (oy > max_y) max_y = oy;
        }
    }

    const int x0 = static_cast<int>(std::floor(min_x));
    const int x1 = static_cast<int>(std::ceil(max_x));
    const int y0 = static_cast<int>(std::floor(min_y));
    const int y1 = static_cast<int>(std::ceil(max_y));
    if (x0 >= canvas_w || x1 < 0 || y0 >= canvas_h || y1 < 0) return;

    const int xlo = std::max(0, x0);
    const int xhi = std::min(canvas_w - 1, x1);
    const int ylo = std::max(0, y0);
    const int yhi = std::min(canvas_h - 1, y1);
    if (xlo > xhi || ylo > yhi) return;

    const std::size_t dst_stride = static_cast<std::size_t>(canvas_w) * 4u;
    const float opacity = clip.opacity;
    for (int oy = ylo; oy <= yhi; ++oy) {
        const std::size_t drow = static_cast<std::size_t>(oy) * dst_stride;
        for (int ox = xlo; ox <= xhi; ++ox) {
            // Inverse map (reverse of scale -> flip -> rotate -> position).
            const double ux = ox - pxx - px;
            const double uy = oy - pyy - py;
            const double vx = ux * cs + uy * sn;
            const double vy = -ux * sn + uy * cs;
            const double wx = vx * fx;
            const double wy = vy * fy;
            const double zx = wx / sx_;
            const double zy = wy / sy_;
            const double param_x = zx + px;
            const double param_y = zy + py;
            const double nx = (param_x - bx) / base_w;
            const double ny = (param_y - by) / base_h;
            if (nx < 0.0 || nx > 1.0 || ny < 0.0 || ny > 1.0) continue;
            const int sy = std::clamp(static_cast<int>(ny * src.height), 0, src.height - 1);
            const int sx = std::clamp(static_cast<int>(nx * src.width), 0, src.width - 1);
            const std::size_t so =
                static_cast<std::size_t>(sy) * src.stride + static_cast<std::size_t>(sx) * 4u;
            std::size_t dpo = drow + static_cast<std::size_t>(ox) * 4u;
            if (opacity >= 1.0f && mode == 0) {
                canvas[dpo + 0] = src.rgba[so + 0];
                canvas[dpo + 1] = src.rgba[so + 1];
                canvas[dpo + 2] = src.rgba[so + 2];
            } else {
                canvas[dpo + 0] = blend_channel(mode, opacity, canvas[dpo + 0], src.rgba[so + 0]);
                canvas[dpo + 1] = blend_channel(mode, opacity, canvas[dpo + 1], src.rgba[so + 1]);
                canvas[dpo + 2] = blend_channel(mode, opacity, canvas[dpo + 2], src.rgba[so + 2]);
            }
            canvas[dpo + 3] = 255;
        }
    }
}

}  // namespace

VideoFramePtr render_video_frame(const Project& project, int64_t tl_frame, int width,
                                 int height, int max_dim,
                                 const struct AVBufferRef* hw_device_ctx) {
    if (width <= 0 || height <= 0) {
        log::log_error("render_video_frame: BAD_ARGS w=%d h=%d", width, height);
        return nullptr;
    }
    const Sequence& seq = project.sequence;
    CANVAS_LOG("render_video_frame: tl_frame=%lld %dx%d tracks=%zu",
           (long long)tl_frame, width, height, seq.video_tracks.size());

    auto canvas = std::make_shared<VideoFrame>();
    canvas->width = width;
    canvas->height = height;
    canvas->stride = static_cast<std::size_t>(width) * 4u;
    canvas->rgba.assign(canvas->stride * static_cast<std::size_t>(height), 0);
    canvas->frame_number = tl_frame;

    // Decode each enabled video track's clip at this frame, top (last) to
    // bottom (first), compositing onto the canvas.
    for (std::size_t i = seq.video_tracks.size(); i-- > 0;) {
        const auto& track = seq.video_tracks[i];
        if (track.locked) continue;
        const Clip* clip = track.clip_at(tl_frame);
        if (!clip || !clip->enabled || clip->media < 0) continue;

        const MediaEntry* m = project.media_by_id(clip->media);
        if (!m) continue;

        std::string err;
        VideoDecoder dec;
        if (!dec.open(m->path, &err, hw_device_ctx)) {
            log::log_error("render_video_frame: decoder open FAILED track=%zu media=%d path='%s' err='%s'",
                            i, clip->media, m->path.c_str(), err.c_str());
            continue;
        }

        const int64_t src_frame = clip_src_frame(*clip, tl_frame);
        // Pillarbox/letterbox the source to fit the canvas preserving aspect.
        const double src_ar = dec.width() > 0 && dec.height() > 0
                                  ? static_cast<double>(dec.width()) / dec.height()
                                  : 1.0;
        int dst_w = width, dst_h = height;
        if (src_ar > 0.0) {
            if (static_cast<double>(width) / height > src_ar) {
                dst_h = height;
                dst_w = std::max(1, static_cast<int>(std::llround(height * src_ar)));
            } else {
                dst_w = width;
                dst_h = std::max(1, static_cast<int>(std::llround(width / src_ar)));
            }
        }
        dst_w = std::min(dst_w, width);
        dst_h = std::min(dst_h, height);
        const int dx = (width - dst_w) / 2;
        const int dy = (height - dst_h) / 2;

        // Decode at full res (max_dim 0). For speed we allow a reduced cache,
        // but correctness + quality matter most for export.
        auto frame = dec.decode_to_frame(src_frame, 0);
        if (frame) {
            blit_rgba_transformed(*frame, canvas->rgba, width, height, dst_w, dst_h,
                                  dx, dy, *clip);
        } else {
            CANVAS_LOG("render_video_frame: decode FAILED track=%zu src_frame=%lld media=%d",
                   i, (long long)src_frame, clip->media);
        }
    }
    return canvas;
}

namespace {

struct AudioTrackState {
    const Track* track = nullptr;
    std::unique_ptr<AudioDecoder> dec;
    const Clip* clip = nullptr;
};

}  // namespace

RenderSession::~RenderSession() { end(); }

void RenderSession::end() {
    tracks_.clear();
    audio_tracks_.clear();
}

bool RenderSession::begin(const Project& project, int width, int height,
                          const struct AVBufferRef* hw_device_ctx) {
    end();
    width_ = width;
    height_ = height;
    hw_device_ctx_ = hw_device_ctx;
    project_ = &project;
    if (width_ <= 0 || height_ <= 0) {
        log::log_error("RenderSession::begin FAILED w=%d h=%d", width, height);
        return false;
    }
    const auto& seq = project.sequence;
    for (const auto& track : seq.video_tracks) {
        if (track.locked) continue;
        TrackDecoder td;
        td.track = &track;
        tracks_.push_back(std::move(td));
    }
    for (const auto& track : seq.audio_tracks) {
        if (track.locked) continue;
        AudioTrackDecoder td;
        td.track = &track;
        audio_tracks_.push_back(std::move(td));
    }
    CANVAS_LOG("RenderSession::begin OK w=%d h=%d video_tracks=%zu audio_tracks=%zu",
           width, height, tracks_.size(), audio_tracks_.size());
    return true;
}

VideoFramePtr RenderSession::frame(int64_t tl_frame) {
    if (width_ <= 0 || height_ <= 0) return nullptr;

    auto canvas = std::make_shared<VideoFrame>();
    canvas->width = width_;
    canvas->height = height_;
    canvas->stride = static_cast<std::size_t>(width_) * 4u;
    canvas->rgba.assign(canvas->stride * static_cast<std::size_t>(height_), 0);
    canvas->frame_number = tl_frame;

    // Composite each enabled video track, top (last) to bottom (first), reusing
    // a persistent decoder per track so we never re-open the media every frame.
    for (std::size_t i = tracks_.size(); i-- > 0;) {
        TrackDecoder& td = tracks_[i];
        const Track& track = *td.track;
        const Clip* clip = track.clip_at(tl_frame);
        if (!clip || !clip->enabled || clip->media < 0) {
            td.dec.reset();
            td.active_clip = nullptr;
            continue;
        }

        // Re-open the decoder only when the active clip/media changes.
        if (!td.dec || !td.dec->is_open() || td.active_media != clip->media ||
            td.active_tl_in != clip->tl_in) {
            const MediaEntry* m = project_ ? project_->media_by_id(clip->media) : nullptr;
            if (!m) {
                CANVAS_LOG("RenderSession::frame: no media for clip media=%d", clip->media);
                td.dec.reset(); continue;
            }
            std::string err;
            td.dec.reset();
            td.dec = std::make_unique<VideoDecoder>();
            if (!td.dec->open(m->path, &err, hw_device_ctx_)) {
                log::log_error("RenderSession::frame: decoder open FAILED media=%d path='%s' err='%s'",
                                clip->media, m->path.c_str(), err.c_str());
                td.dec.reset();
                continue;
            }
            td.active_clip = clip;
            td.active_media = clip->media;
            td.active_tl_in = clip->tl_in;
        }

        const int64_t src_frame = clip->src_in + (tl_frame - clip->tl_in);
        VideoFramePtr decoded = td.dec->decode_to_frame(src_frame, 0);
        if (!decoded) {
            CANVAS_LOG("RenderSession::frame: decode FAILED track=%zu src_frame=%lld media=%d",
                   i, (long long)src_frame, clip->media);
            continue;
        }

        // Pillarbox/letterbox the source to fit the canvas preserving aspect.
        const double dar = decoded->width > 0
                               ? static_cast<double>(decoded->width) / decoded->height
                               : 1.0;
        int dst_w = width_, dst_h = height_;
        if (dar > 0.0 && decoded->width > 0) {
            if (static_cast<double>(width_) / height_ > dar) {
                dst_h = height_;
                dst_w = std::max(1, static_cast<int>(std::llround(height_ * dar)));
            } else {
                dst_w = width_;
                dst_h = std::max(1, static_cast<int>(std::llround(width_ / dar)));
            }
        }
        dst_w = std::min(dst_w, width_);
        dst_h = std::min(dst_h, height_);
        blit_rgba(*decoded, canvas->rgba, width_, height_, dst_w, dst_h,
                  (width_ - dst_w) / 2, (height_ - dst_h) / 2);
    }

    // Per-clip edge fades applied to the composited canvas: fade-in from black at
    // the top clip's head, fade-out to black at its tail when no clip follows the
    // cut. Both blend the whole frame toward black by a factor from the window.
    float fade = 1.0f;
    const Sequence& seq_ = project_ ? project_->sequence : Sequence{};
    const Clip* a_ = nullptr;
    for (std::size_t i = seq_.video_tracks.size(); i-- > 0;) {
        const Track& t_ = seq_.video_tracks[i];
        if (t_.locked) continue;
        const Clip* c_ = t_.clip_at(tl_frame);
        if (c_ && c_->enabled && c_->media >= 0) { a_ = c_; break; }
    }
    if (a_) {
        const bool has_b_ = [&] {
            for (const auto& t_ : seq_.video_tracks) {
                if (t_.locked) continue;
                for (const auto& c_ : t_.clips)
                    if (c_.id != a_->id && c_.tl_in == a_->tl_out) return true;
            }
            return false;
        }();
        if (a_->has_transition_in() && !is_audio_transition(a_->transition_in) &&
            a_->transition_in_duration > 0 && tl_frame >= a_->tl_in &&
            tl_frame < a_->tl_in + a_->transition_in_duration) {
            fade *= static_cast<float>(tl_frame - a_->tl_in) /
                    static_cast<float>(a_->transition_in_duration);
        }
        if (a_->has_transition_out() && !is_audio_transition(a_->transition_out) &&
            !has_b_ && a_->transition_out_duration > 0 &&
            tl_frame >= a_->tl_out - a_->transition_out_duration &&
            tl_frame < a_->tl_out) {
            fade *= 1.0f - static_cast<float>(tl_frame - (a_->tl_out - a_->transition_out_duration)) /
                            static_cast<float>(a_->transition_out_duration);
        }
    }
    if (fade < 1.0f && fade > 0.0f) {
        const std::size_t n = canvas->rgba.size();
        for (std::size_t i = 0; i + 3 < n; i += 4) {
            canvas->rgba[i + 0] = static_cast<uint8_t>(canvas->rgba[i + 0] * fade);
            canvas->rgba[i + 1] = static_cast<uint8_t>(canvas->rgba[i + 1] * fade);
            canvas->rgba[i + 2] = static_cast<uint8_t>(canvas->rgba[i + 2] * fade);
        }
    }
    return canvas;
}

bool RenderSession::frame_gpu(int64_t tl_frame, GpuFrameInfo* out) {
    if (!out) return false;
    out->valid = false;
    if (width_ <= 0 || height_ <= 0 || !hw_device_ctx_ || !project_) {
        CANVAS_LOG("frame_gpu: preconditions failed w=%d h=%d hw=%p proj=%p",
               width_, height_, (const void*)hw_device_ctx_, (const void*)project_);
        return false;
    }

    // Engage only when exactly one video clip is enabled at this frame (the common
    // no-overlap export case); any overlap/transform falls back to the CPU
    // compositor to guarantee identical semantics.
    const Clip* the_clip = nullptr;
    {
        int found = 0;
        const auto& seq = project_->sequence;
        for (const auto& track : seq.video_tracks) {
            if (track.locked) continue;
            const Clip* c = track.clip_at(tl_frame);
            if (c && c->enabled && c->media >= 0) {
                ++found;
                the_clip = c;
            }
        }
        if (found != 1 || !the_clip) {
            CANVAS_LOG("frame_gpu: clip_count=%d at tl_frame=%lld (need exactly 1)",
                   found, (long long)tl_frame);
            return false;
        }
    }

    // Single-clip edge fades must go through the CPU compositor, which applies
    // the black-factor blend; the raw GPU NV12 path cannot express it, so bail
    // whenever tl_frame is inside such a window.
    {
        const Clip* f = the_clip;
        const bool in_in = f->has_transition_in() && !is_audio_transition(f->transition_in) &&
                           f->transition_in_duration > 0 && tl_frame >= f->tl_in &&
                           tl_frame < f->tl_in + f->transition_in_duration;
        bool has_b_after = false;
        for (const auto& track : project_->sequence.video_tracks) {
            if (track.locked) continue;
            for (const auto& cc : track.clips)
                if (cc.id != f->id && cc.tl_in == f->tl_out) { has_b_after = true; break; }
            if (has_b_after) break;
        }
        const bool in_out = f->has_transition_out() && !is_audio_transition(f->transition_out) &&
                            !has_b_after && f->transition_out_duration > 0 &&
                            tl_frame >= f->tl_out - f->transition_out_duration &&
                            tl_frame < f->tl_out;
        if (in_in || in_out) {
            CANVAS_LOG("frame_gpu: clip id=%lld tl_frame=%lld in single-clip fade window, using CPU path",
                   (long long)f->id, (long long)tl_frame);
            return false;
        }
    }

    // Locate the persistent decoder for the track holding this clip.
    TrackDecoder* td = nullptr;
    for (auto& t : tracks_) {
        if (t.track && t.track->clip_at(tl_frame)) { td = &t; break; }
    }
    if (!td) {
        CANVAS_LOG("frame_gpu: no track decoder for tl_frame=%lld", (long long)tl_frame);
        return false;
    }
    const Track& track = *td->track;
    const Clip* clip = track.clip_at(tl_frame);
    if (!clip || !clip->enabled || clip->media < 0) return false;

    const MediaEntry* m = project_->media_by_id(clip->media);
    if (!m) return false;

    if (!td->dec || !td->dec->is_open() || td->active_media != clip->media ||
        td->active_tl_in != clip->tl_in) {
        std::string err;
        td->dec.reset();
        td->dec = std::make_unique<VideoDecoder>();
        if (!td->dec->open(m->path, &err, hw_device_ctx_)) {
            CANVAS_LOG("frame_gpu: decoder open FAILED path='%s' err='%s'", m->path.c_str(), err.c_str());
            td->dec.reset();
            return false;
        }
        td->active_clip = clip;
        td->active_media = clip->media;
        td->active_tl_in = clip->tl_in;
    }

    const int64_t src_frame = clip->src_in + (tl_frame - clip->tl_in);
    const AVFrame* hw = td->dec->decode_to_hw(src_frame);
    if (!hw || !hw->hw_frames_ctx || hw->width <= 0 || hw->height <= 0) {
        CANVAS_LOG("frame_gpu: decode_to_hw FAILED tl_frame=%lld src_frame=%lld hw=%p ctx=%p w=%d h=%d",
               (long long)tl_frame, (long long)src_frame,
               (const void*)hw, hw ? (const void*)hw->hw_frames_ctx : nullptr,
               hw ? hw->width : 0, hw ? hw->height : 0);
        return false;
    }
    // The frame returned may overshoot src_frame (>= matching); record the true
    // decoded number so the caller can detect duplicate/out-of-order emissions.
    out->src_frame = td->dec->current_frame() - 1;

    // Letterbox the source into the canvas preserving its aspect (same rule as
    // blit_rgba) so the GPU result matches the CPU framing.
    const double dar = static_cast<double>(hw->width) / hw->height;
    int dst_w = width_, dst_h = height_;
    if (dar > 0.0) {
        if (static_cast<double>(width_) / height_ > dar) {
            dst_h = height_;
            dst_w = std::max(1, static_cast<int>(std::llround(height_ * dar)));
        } else {
            dst_w = width_;
            dst_h = std::max(1, static_cast<int>(std::llround(width_ / dar)));
        }
    }
    dst_w = std::min(dst_w, width_);
    dst_h = std::min(dst_h, height_);

    out->srcY = reinterpret_cast<uintptr_t>(hw->data[0]);
    out->srcUV = reinterpret_cast<uintptr_t>(hw->data[1]);
    out->srcYPitch = static_cast<std::size_t>(hw->linesize[0]);
    out->srcUVPitch = static_cast<std::size_t>(hw->linesize[1]);
    out->srcW = hw->width;
    out->srcH = hw->height;
    out->outW = width_;
    out->outH = height_;
    out->dstW = dst_w;
    out->dstH = dst_h;
    out->dx = (width_ - dst_w) / 2;
    out->dy = (height_ - dst_h) / 2;
    out->source = hw;
    out->valid = true;
    return true;
}

AudioChunkPtr render_audio_chunk(const Project& project, int64_t tl_sample, int num_frames,
                                 int out_sample_rate, int out_channels, double fps,
                                 RenderControl* control) {
    (void)control;
    if (num_frames <= 0 || out_sample_rate <= 0 || out_channels <= 0) {
        log::log_error("render_audio_chunk: BAD_ARGS frames=%d rate=%d ch=%d",
                        num_frames, out_sample_rate, out_channels);
        return nullptr;
    }
    const Sequence& seq = project.sequence;
    CANVAS_LOG("render_audio_chunk: tl_sample=%lld frames=%d rate=%d ch=%d",
           (long long)tl_sample, num_frames, out_sample_rate, out_channels);

    auto out = std::make_shared<AudioChunk>();
    out->start_sample = tl_sample;
    out->sample_rate = out_sample_rate;
    out->channels = out_channels;
    out->samples.assign(static_cast<std::size_t>(num_frames) *
                            static_cast<std::size_t>(out_channels),
                        0.0f);

    // Decode each audible audio track and mix (sum) its samples in place.
    // Tracks are looped from the top (later tracks win nothing here — audio
    // sums, so order is irrelevant); locked tracks are still rendered
    // (locked = read-only, audio stays audible, mirroring playback).
    const bool any_solo = audio_mix::any_solo(seq.audio_tracks);
    for (const auto& track : seq.audio_tracks) {
        if (track.muted || (any_solo && !track.solo)) continue;
        // Determine which timeline frame range this audio chunk covers.
        const double frame_at_sample =
            (static_cast<double>(tl_sample) / out_sample_rate) * fps;
        const int64_t tl_frame = static_cast<int64_t>(std::floor(frame_at_sample));
        const Clip* clip = track.clip_at(tl_frame);
        if (!clip || !clip->enabled || clip->media < 0) continue;

        const MediaEntry* m = project.media_by_id(clip->media);
        if (!m) continue;

        AudioDecoder dec;
        std::string err;
        if (!dec.open(m->path) || !dec.has_audio()) {
            CANVAS_LOG("render_audio_chunk: open/audio FAILED has_audio=%d media=%d path='%s'",
                   (int)dec.has_audio(), clip->media, m->path.c_str());
            continue;
        }

        const double media_fps = media_fps_for(project, *clip);
        if (media_fps <= 0.0) continue;

        // Frame at the start of this chunk.
        const int64_t start_tl_frame = static_cast<int64_t>(
            std::floor((static_cast<double>(tl_sample) / out_sample_rate) * fps));
        const int64_t src_frame = clip_src_frame(*clip, start_tl_frame);
        const int64_t start_media_sample =
            (src_frame >= 0)
                ? static_cast<int64_t>(std::llround(
                      static_cast<double>(src_frame) / media_fps * out_sample_rate))
                : 0;

        auto chunk = dec.decode(start_media_sample, num_frames, out_sample_rate);
        if (!chunk || chunk->samples.empty()) {
            CANVAS_LOG("render_audio_chunk: decode EMPTY media=%d src_frame=%lld media_sample=%lld",
                   clip->media, (long long)src_frame, (long long)start_media_sample);
            continue;
        }

        const int src_ch = chunk->channels;
        // Per-output-frame gain from the clip's audio IN/OUT transitions
        // (audio_fade_gain returns 1.0 when no audio fade touches the frame).
        std::vector<float> gains(static_cast<std::size_t>(num_frames));
        for (int k = 0; k < num_frames; ++k) {
            const int64_t frm = start_tl_frame + static_cast<int64_t>(
                static_cast<double>(k) / out_sample_rate * fps);
            gains[static_cast<std::size_t>(k)] = audio_fade_gain(*clip, frm);
        }
        // Mix: sum with the clip's volume and pan balance.
        float gl = 1.0f;
        float gr = 1.0f;
        audio_mix::pan_gains(clip->pan, gl, gr);
        mix_audio_chunk(out->samples, chunk->samples, src_ch, out_channels, gains,
                        audio_mix::db_to_gain(clip->volume_db), gl, gr);
    }
    return out;
}

AudioChunkPtr RenderSession::audio_chunk(int64_t tl_sample, int num_frames,
                                         int out_sample_rate, int out_channels, double fps) {
    if (num_frames <= 0 || out_sample_rate <= 0 || out_channels <= 0) {
        log::log_error("RenderSession::audio_chunk BAD_ARGS frames=%d rate=%d ch=%d",
                        num_frames, out_sample_rate, out_channels);
        return nullptr;
    }
    if (!project_) {
        CANVAS_LOG("RenderSession::audio_chunk no project");
        return nullptr;
    }

    auto out = std::make_shared<AudioChunk>();
    out->start_sample = tl_sample;
    out->sample_rate = out_sample_rate;
    out->channels = out_channels;
    out->samples.assign(static_cast<std::size_t>(num_frames) *
                            static_cast<std::size_t>(out_channels),
                        0.0f);
    CANVAS_LOG("RenderSession::audio_chunk tl_sample=%lld frames=%d rate=%d ch=%d",
           (long long)tl_sample, num_frames, out_sample_rate, out_channels);

    // Round to nearest, not floor: the float product at an exact frame boundary
    // can evaluate a hair below the integer (98400 samples @60fps -> 122.9999998),
    // and flooring that systematically requests the previous frame every chunk ->
    // a persistent 1-frame backward drift that force-re-seeks -> repeated audio.
    const int64_t start_tl_frame = std::llround(
        (static_cast<double>(tl_sample) / out_sample_rate) * fps);

    // Mix each audible audio track in place, reusing the persistent per-track
    // AudioDecoder so sequential chunks advance one continuous decode stream
    // (no re-open/resampler re-prime per chunk — that produced repeated audio).
    // Locked tracks are still rendered (locked = read-only, still audible),
    // mirroring playback semantics.
    const bool any_solo = audio_mix::any_solo(project_->sequence.audio_tracks);
    for (AudioTrackDecoder& atd : audio_tracks_) {
        const Track& track = *atd.track;
        if (track.muted || (any_solo && !track.solo)) {
            atd.dec.reset();
            atd.active_clip = nullptr;
            continue;
        }
        const Clip* clip = track.clip_at(start_tl_frame);
        if (!clip || !clip->enabled || clip->media < 0) {
            atd.dec.reset();
            atd.active_clip = nullptr;
            continue;
        }

        if (!atd.dec || !atd.dec->has_audio() || atd.active_media != clip->media ||
            atd.active_tl_in != clip->tl_in) {
            const MediaEntry* m = project_->media_by_id(clip->media);
            if (!m) {
                CANVAS_LOG("RenderSession::audio_chunk no media for clip media=%d", clip->media);
                atd.dec.reset(); continue;
            }
            atd.dec.reset();
            atd.dec = std::make_unique<AudioDecoder>();
            if (!atd.dec->open(m->path) || !atd.dec->has_audio()) {
                log::log_error("RenderSession::audio_chunk decoder open FAILED media=%d path='%s'",
                                clip->media, m->path.c_str());
                atd.dec.reset();
                continue;
            }
            atd.active_clip = clip;
            atd.active_media = clip->media;
            atd.active_tl_in = clip->tl_in;
        }

        const double media_fps = media_fps_for(*project_, *clip);
        if (media_fps <= 0.0) continue;
        const int64_t src_frame = clip_src_frame(*clip, start_tl_frame);
        const int64_t start_media_sample =
            (src_frame >= 0)
                ? static_cast<int64_t>(std::llround(
                      static_cast<double>(src_frame) / media_fps * out_sample_rate))
                : 0;

        auto chunk = atd.dec->decode(start_media_sample, num_frames, out_sample_rate);
        if (!chunk || chunk->samples.empty()) {
            CANVAS_LOG("RenderSession::audio_chunk decode EMPTY media=%d src=%lld sample=%lld",
                   clip->media, (long long)src_frame, (long long)start_media_sample);
            continue;
        }

        // [AUDIO-DIAG] unconditional trace (mirrors the [dbg]/[FRAME-DIAG] hooks):
//   - <SHORTFALL> decoded fewer samples than requested. The exporter advances
//                 by max(got,req), so a shortfall runs AHEAD of real audio; a
//                 later hard-seek past the decoder's forward tolerance repeats
//                 audio (Matroska-PCM far-seek).
//   - <BACKJUMP>  media_sample went backward/stalled (a true loop).
        const std::size_t _src_ch = static_cast<std::size_t>(chunk->channels);
        const std::size_t _n = chunk->samples.size();
        const int _got = (int)(_n / _src_ch);
        const bool _short = _n > 0 && _got > 0 && _got < num_frames;
        static int64_t _d_last_sample = INT64_MIN;
        static long _d_call = 0;
        const bool _d_back = (_d_call > 0) && (_n > 0) && (start_media_sample <= _d_last_sample);
        if (_d_call < 60 || _short || _d_back) {
            std::fprintf(stderr,
                         "[AUDIO-DIAG] call=%ld tl_sample=%lld tl_frame=%lld src_frame=%lld "
                         "media_sample=%lld req=%d got=%d%s%s\n",
                         _d_call, (long long)tl_sample, (long long)start_tl_frame,
                         (long long)src_frame, (long long)start_media_sample,
                         (int)num_frames, _got,
                         _short ? " <SHORTFALL>" : "", _d_back ? " <BACKJUMP>" : "");
        }
        _d_last_sample = start_media_sample;
        ++_d_call;

        const int src_ch = chunk->channels;
        // Per-output-frame gain from the clip's audio IN/OUT transitions
        // (audio_fade_gain returns 1.0 when no audio fade touches the frame).
        std::vector<float> gains(static_cast<std::size_t>(num_frames));
        for (int k = 0; k < num_frames; ++k) {
            const int64_t frm = start_tl_frame + static_cast<int64_t>(
                static_cast<double>(k) / out_sample_rate * fps);
            gains[static_cast<std::size_t>(k)] = audio_fade_gain(*clip, frm);
        }
        // Mix: sum with the clip's volume and pan balance.
        float gl = 1.0f;
        float gr = 1.0f;
        audio_mix::pan_gains(clip->pan, gl, gr);
        mix_audio_chunk(out->samples, chunk->samples, src_ch, out_channels, gains,
                        audio_mix::db_to_gain(clip->volume_db), gl, gr);
    }
    return out;
}

}  // namespace canvas::core
