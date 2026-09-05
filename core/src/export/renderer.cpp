#include "canvas/core/export/renderer.hpp"

#include "canvas/core/media/audio_decoder.hpp"
#include "canvas/core/media/video_decoder.hpp"
#include "canvas/core/timeline/audio_fade.hpp"
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
            blit_rgba(*frame, canvas->rgba, width, height, dst_w, dst_h, dx, dy);
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

    // Decode each enabled audio track and mix (sum) its samples in place.
    for (const auto& track : seq.audio_tracks) {
        if (track.locked) continue;
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
        const std::size_t n = chunk->samples.size();
        // Per-output-frame gain from the clip's audio IN/OUT transitions
        // (audio_fade_gain returns 1.0 when no audio fade touches the frame).
        std::vector<float> gains(static_cast<std::size_t>(num_frames));
        for (int k = 0; k < num_frames; ++k) {
            const int64_t frm = start_tl_frame + static_cast<int64_t>(
                static_cast<double>(k) / out_sample_rate * fps);
            gains[static_cast<std::size_t>(k)] = audio_fade_gain(*clip, frm);
        }
        // Mix: sum left into left, right into right; down/up-mix simply.
        for (std::size_t s = 0; s < n; ++s) {
            const std::size_t src_sc = s % static_cast<std::size_t>(src_ch);
            const std::size_t dst_sc =
                std::min<std::size_t>(src_sc, static_cast<std::size_t>(out_channels) - 1);
            const std::size_t frame_idx = s / static_cast<std::size_t>(src_ch);
            const std::size_t frame_ch = frame_idx * static_cast<std::size_t>(out_channels);
            if (frame_ch + dst_sc < out->samples.size())
                out->samples[frame_ch + dst_sc] += chunk->samples[s] * gains[frame_idx];
        }
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

    // Mix each enabled audio track in place, reusing the persistent per-track
    // AudioDecoder so sequential chunks advance one continuous decode stream
    // (no re-open/resampler re-prime per chunk — that produced repeated audio).
    for (AudioTrackDecoder& atd : audio_tracks_) {
        const Track& track = *atd.track;
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
        const std::size_t n = chunk->samples.size();
        // Per-output-frame gain from the clip's audio IN/OUT transitions
        // (audio_fade_gain returns 1.0 when no audio fade touches the frame).
        std::vector<float> gains(static_cast<std::size_t>(num_frames));
        for (int k = 0; k < num_frames; ++k) {
            const int64_t frm = start_tl_frame + static_cast<int64_t>(
                static_cast<double>(k) / out_sample_rate * fps);
            gains[static_cast<std::size_t>(k)] = audio_fade_gain(*clip, frm);
        }
        for (std::size_t s = 0; s < n; ++s) {
            const std::size_t src_sc = s % static_cast<std::size_t>(src_ch);
            const std::size_t dst_sc =
                std::min<std::size_t>(src_sc, static_cast<std::size_t>(out_channels) - 1);
            const std::size_t frame_idx = s / static_cast<std::size_t>(src_ch);
            const std::size_t frame_ch = frame_idx * static_cast<std::size_t>(out_channels);
            if (frame_ch + dst_sc < out->samples.size())
                out->samples[frame_ch + dst_sc] += chunk->samples[s] * gains[frame_idx];
        }
    }
    return out;
}

}  // namespace canvas::core
