#include "canvas/core/media/video_decoder.hpp"
#include "canvas/core/util/log.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>

namespace canvas::core {

VideoDecoder::~VideoDecoder() { close(); }

bool VideoDecoder::open(const std::string& path, std::string* error,
                        const AVBufferRef* hw_device_ctx) {
    close();
    path_ = path;

    AVFormatContext* ctx = nullptr;
    if (avformat_open_input(&ctx, path.c_str(), nullptr, nullptr) < 0) {
        if (error) *error = "failed to open '" + path + "'";
        return false;
    }
    fmt_ctx_ = ctx;

    if (avformat_find_stream_info(fmt_ctx_, nullptr) < 0) {
        if (error) *error = "failed to read stream info";
        close();
        return false;
    }

    video_stream_ = av_find_best_stream(fmt_ctx_, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (video_stream_ < 0) {
        if (error) *error = "no video stream found";
        close();
        return false;
    }

    const AVStream* stream = fmt_ctx_->streams[video_stream_];
    const AVCodecID codec_id = stream->codecpar->codec_id;

    // Choose the decoder. With a hardware device supplied, prefer a decoder that
    // exposes an AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX config for that device
    // type. avcodec_find_decoder() alone can return a pure-software decoder
    // (e.g. libdav1d for AV1) exposing no hw config, which would silently cap
    // export/scrub at CPU speed despite a hardware decoder existing. Fall back
    // to the default software decoder when no hw decoder or device is found.
    const AVHWDeviceType dev_type = hw_device_ctx
        ? reinterpret_cast<const AVHWDeviceContext*>(hw_device_ctx->data)->type
        : AV_HWDEVICE_TYPE_NONE;
    const AVCodec* codec = nullptr;
    if (dev_type != AV_HWDEVICE_TYPE_NONE) {
        bool found_hw = false;
        void* iter = nullptr;
        while (const AVCodec* cand = av_codec_iterate(&iter)) {
            if (!av_codec_is_decoder(cand) || cand->type != AVMEDIA_TYPE_VIDEO ||
                cand->id != codec_id)
                continue;
            for (int i = 0; !found_hw; ++i) {
                const AVCodecHWConfig* cfg = avcodec_get_hw_config(cand, i);
                if (!cfg) break;
                if ((cfg->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX) &&
                    cfg->device_type == dev_type) {
                    codec = cand;
                    found_hw = true;
                }
            }
            if (found_hw) break;
        }
    }
    if (!codec) codec = avcodec_find_decoder(codec_id);
    if (!codec) {
        if (error) *error = "no decoder for codec";
        close();
        return false;
    }

    codec_ctx_ = avcodec_alloc_context3(codec);
    if (!codec_ctx_ || avcodec_parameters_to_context(codec_ctx_, stream->codecpar) < 0) {
        if (error) *error = "failed to initialize decoder";
        close();
        return false;
    }

    // If a shared hardware device is available and this codec supports it, enable
    // hardware decode. The hw pixel format is remembered so the download path
    // (make_rgba_frame) knows how to pull data back to the CPU.
    hw_pix_fmt_ = AV_PIX_FMT_NONE;
    hw_avail_ = false;
    if (hw_device_ctx) {
        const AVBufferRef* dev = hw_device_ctx;
        const AVHWDeviceContext* hwctx =
            reinterpret_cast<const AVHWDeviceContext*>(dev->data);
        const AVHWDeviceType dev_type = hwctx->type;
        for (int i = 0;; ++i) {
            const AVCodecHWConfig* config = avcodec_get_hw_config(codec, i);
            if (!config) break;
            if (config->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX &&
                config->device_type == dev_type) {
                codec_ctx_->hw_device_ctx = av_buffer_ref(hw_device_ctx);
                hw_pix_fmt_ = config->pix_fmt;
                break;
            }
        }
        if (hw_pix_fmt_ != AV_PIX_FMT_NONE) {
            codec_ctx_->get_format = [](AVCodecContext* c, const AVPixelFormat* pix_fmts) {
                for (const AVPixelFormat* p = pix_fmts; *p != AV_PIX_FMT_NONE; ++p) {
                    if (*p == static_cast<AVPixelFormat>(reinterpret_cast<intptr_t>(c->opaque)))
                        return *p;
                }
                return AV_PIX_FMT_NONE;
            };
            // Stash the hw pixel format for the get_format callback via opaque.
            codec_ctx_->opaque = reinterpret_cast<void*>(static_cast<intptr_t>(hw_pix_fmt_));
        }
    }

    if (avcodec_open2(codec_ctx_, codec, nullptr) < 0) {
        if (error) *error = "failed to initialize decoder";
        close();
        return false;
    }
    codec_ctx_->thread_count = 0;

    // Hardware decode is on when we found an AVCodecHWConfig for the shared device
    // (hw_pix_fmt_ set). Hardware frames carry a hw_frames_ctx and get
    // downloaded in make_rgba_frame; if the codec can't actually decode in
    // hardware, FFmpeg transparently emits software frames instead.
    hw_avail_ = hw_pix_fmt_ != AV_PIX_FMT_NONE;

    av_frame_ = av_frame_alloc();
    packet_ = av_packet_alloc();
    if (!av_frame_ || !packet_) {
        if (error) *error = "out of memory";
        close();
        return false;
    }

    stream_tb_ = stream->time_base;
    width_ = codec_ctx_->width;
    height_ = codec_ctx_->height;

    const AVRational fr = stream->avg_frame_rate.num != 0 ? stream->avg_frame_rate : stream->r_frame_rate;
    frame_rate_ = fr.num > 0 ? av_q2d(fr) : 0.0;
    duration_seconds_ = fmt_ctx_->duration > 0
        ? static_cast<double>(fmt_ctx_->duration) / AV_TIME_BASE
        : (stream->duration > 0 ? static_cast<double>(stream->duration) * av_q2d(stream_tb_) : 0.0);
    total_frames_ = frame_rate_ > 0.0 && duration_seconds_ > 0.0
        ? static_cast<int64_t>(std::llround(duration_seconds_ * frame_rate_))
        : -1;
    // Seed the decode clamp from the open-time duration estimate so behavior is
    // unchanged when the container hands a duration; refine_last_frame() and the
    // EOF sites tighten it later when only the stream extent is known.
    last_frame_ = total_frames_ > 0 ? total_frames_ - 1 : -1;
    CANVAS_LOG("decode open: '%s' %dx%d fps=%.3f dur=%.3fs hw=%s hw_pix=%d",
           path.c_str(), width_, height_, frame_rate_, duration_seconds_,
           hw_avail_ ? "yes" : "no", hw_pix_fmt_);

    // --- Optional audio stream ---
    audio_stream_ = -1;
    audio_sample_rate_ = 0;
    audio_channels_ = 0;
    audio_samples_total_ = 0;
    // Open a *separate* demuxer for audio. Video lookahead advances the shared
    // fmt_ctx_ and discards non-video packets, so audio needs its own container
    // (and seek position) or the mutual seeks drag video below a frame per second.
    AVFormatContext* actx = nullptr;
    if (avformat_open_input(&actx, path.c_str(), nullptr, nullptr) == 0) {
        if (avformat_find_stream_info(actx, nullptr) >= 0) {
            const int a = av_find_best_stream(actx, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
            if (a >= 0) {
                const AVStream* ast = actx->streams[a];
                const AVCodec* ac = avcodec_find_decoder(ast->codecpar->codec_id);
                if (ac) {
                    audio_codec_ = avcodec_alloc_context3(ac);
                    if (audio_codec_ &&
                        avcodec_parameters_to_context(audio_codec_, ast->codecpar) >= 0 &&
                        avcodec_open2(audio_codec_, ac, nullptr) >= 0) {
                        audio_fmt_ctx_ = actx;
                        actx = nullptr;  // ownership transferred
                        audio_stream_ = a;
                        audio_sample_rate_ = audio_codec_->sample_rate > 0 ? audio_codec_->sample_rate : 48000;
                        audio_channels_ = audio_codec_->ch_layout.nb_channels > 0
                                              ? audio_codec_->ch_layout.nb_channels : 2;
                        if (audio_channels_ <= 0) audio_channels_ = 2;
                        audio_samples_total_ = ast->duration > 0 ? ast->duration : -1;
                        audio_tb_ = ast->time_base;
                        audio_frame_ = av_frame_alloc();
                        audio_packet_ = av_packet_alloc();
                        audio_next_sample_ = 0;
                    } else {
                        if (audio_codec_) avcodec_free_context(&audio_codec_);
                        audio_codec_ = nullptr;
                    }
                }
            }
        }
        if (actx) avformat_close_input(&actx);
    }
    CANVAS_LOG("decode open: audio=%s rate=%d ch=%d",
           audio_stream_ >= 0 ? "yes" : "no", audio_sample_rate_, audio_channels_);

    next_frame_ = 0;
    draining_ = false;
    return true;
}

void VideoDecoder::close() {
    CANVAS_LOG("video_decoder: close '%s' hw=%d next_frame=%lld",
           path_.c_str(), (int)hw_avail_, (long long)next_frame_);
    if (packet_) av_packet_free(&packet_);
    if (av_frame_) av_frame_free(&av_frame_);
    if (sws_ctx_) {
        sws_freeContext(sws_ctx_);
        sws_ctx_ = nullptr;
    }
    if (codec_ctx_) avcodec_free_context(&codec_ctx_);
    if (audio_packet_) av_packet_free(&audio_packet_);
    if (audio_frame_) av_frame_free(&audio_frame_);
    if (audio_codec_) avcodec_free_context(&audio_codec_);
    if (swr_ctx_) swr_free(&swr_ctx_);
    if (audio_fmt_ctx_) avformat_close_input(&audio_fmt_ctx_);
    if (fmt_ctx_) avformat_close_input(&fmt_ctx_);
    video_stream_ = -1;
    audio_stream_ = -1;
    audio_sample_rate_ = 0;
    audio_channels_ = 0;
    audio_samples_total_ = 0;
    audio_next_sample_ = 0;
    swr_ctx_ = nullptr;
    audio_codec_ = nullptr;
    audio_frame_ = nullptr;
    audio_packet_ = nullptr;
    audio_fmt_ctx_ = nullptr;
    width_ = 0;
    height_ = 0;
    frame_rate_ = 0.0;
    duration_seconds_ = 0.0;
    total_frames_ = -1;
    next_frame_ = 0;
    draining_ = false;
    stream_tb_ = {0, 1};
    hw_pix_fmt_ = AV_PIX_FMT_NONE;
    hw_avail_ = false;
}

void VideoDecoder::reset_stream_state(const int64_t resume_frame) {
    if (codec_ctx_) avcodec_flush_buffers(codec_ctx_);
    draining_ = false;
    next_frame_ = resume_frame;
}

// Clamps a caller's target frame into the valid source-window [0, last_frame_].
// The upper bound is only an approximation until the stream actually ends (see
// refine_last_frame / the EOF sites), but it is the crucial guard that keeps a
// seek far past the media end from walking the whole GOP chain: even when the
// container hands no duration (opening a paused file), the moment a single frame
// is decoded we know the stream extends at least to frame 0 and a seek target of
// 35k collapses to last_frame_ instead of triggering a many-second forward walk.
int64_t VideoDecoder::clamp_target(const int64_t target) const {
    int64_t t = target < 0 ? 0 : target;
    if (last_frame_ > 0) t = std::min(t, last_frame_);
    return t;
}

// Narrows last_frame_ to the stream's own encoded extent when the container
// duration was missing at open(). Uses the video stream duration when present,
// else the container duration (which FFmpeg fills in from the stream duration
// during find_stream_info), else the frame-rate fallback. Always called on the
// decode path so the clamp tightens as soon as ANY extent is available, without
// waiting for a full demux.
void VideoDecoder::refine_last_frame() {
    if (last_frame_ > 0 || frame_rate_ <= 0.0) return;
    if (fmt_ctx_ && video_stream_ >= 0) {
        const AVStream* st = fmt_ctx_->streams[video_stream_];
        double secs = st->duration > 0 ? static_cast<double>(st->duration) * av_q2d(stream_tb_) : 0.0;
        if (secs <= 0.0 && fmt_ctx_->duration > 0 && fmt_ctx_->duration != AV_NOPTS_VALUE)
            secs = static_cast<double>(fmt_ctx_->duration) / AV_TIME_BASE;
        if (secs <= 0.0 && duration_seconds_ > 0.0) secs = duration_seconds_;
        if (secs > 0.0) last_frame_ = std::max<int64_t>(0, static_cast<int64_t>(
            std::llround(secs * frame_rate_)));
    }
}

VideoFramePtr VideoDecoder::decode_next() {
    if (!codec_ctx_ || frame_rate_ <= 0.0) {
        CANVAS_LOG("video_decoder: decode_next SKIP (codec=%p fps=%.3f)", (void*)codec_ctx_, frame_rate_);
        return nullptr;
    }
    for (;;) {
        const int ret = avcodec_receive_frame(codec_ctx_, av_frame_);
        if (ret == 0) {
            int64_t ticks = av_frame_->best_effort_timestamp;
            if (ticks == AV_NOPTS_VALUE) ticks = av_frame_->pts;
            const bool have_ts = ticks != AV_NOPTS_VALUE;
            const double secs = have_ts
                ? static_cast<double>(ticks) * av_q2d(stream_tb_)
                : static_cast<double>(next_frame_) / frame_rate_;
            const int64_t number =
                std::max<int64_t>(static_cast<int64_t>(std::llround(secs * frame_rate_)), 0);
            auto out = make_rgba_frame(av_frame_, ticks, secs, number);
            av_frame_unref(av_frame_);
            if (!out) continue;
            next_frame_ = number + 1;
            return out;
        }
        if (ret == AVERROR_EOF) {
            // Stream exhausted: the highest produced frame is now known exactly,
            // so future seeks clamp to it (a scrub past the end no longer re-
            // walks the GOP chain to find EOF again).
            if (next_frame_ > 0 && (last_frame_ < 0 || next_frame_ - 1 < last_frame_))
                last_frame_ = next_frame_ - 1;
            CANVAS_LOG("video_decoder: decode_next EOF at frame %lld", (long long)next_frame_);
            return nullptr;
        }
        if (ret != AVERROR(EAGAIN)) {
            log::log_error("video_decoder: decode_next ERROR ret=%d at frame %lld",
                            ret, (long long)next_frame_);
            return nullptr;
        }
        if (draining_) return nullptr;
        for (;;) {
            const int r = av_read_frame(fmt_ctx_, packet_);
            if (r < 0) {
                draining_ = true;
                avcodec_send_packet(codec_ctx_, nullptr);
                break;
            }
            if (packet_->stream_index != video_stream_) {
                av_packet_unref(packet_);
                continue;
            }
            avcodec_send_packet(codec_ctx_, packet_);
            av_packet_unref(packet_);
            break;
        }
    }
}

// Decodes forward from the current position until the target frame, fast-overs
// every intermediate frame (no RGBA conversion, no GPU->CPU copy) and converts
// only the target to RGBA. The intervening decodes are unavoidable for HEVC (no
// decode-time lowres), but skipping per-frame sws + ~14MB of copy traffic for
// the other GOP frames is most of the scrub win.
//
// `max_over` caps the fast-overs for sparse-keyframe media (multi-second GOPs):
// walking ~2850 frames to a far target stalls the worker for seconds and starves
// audio. On the cap, the most recently decoded frame is converted to RGBA and
// returned as an *approximate* preview — a low-res tease, not a frame-accurate
// export — so preview latency stays bounded regardless of GOP density. A later
// yonder-target preview resumes from here, so it never regresses.
std::string VideoDecoder::video_stream_summary() const {
    std::string s = "streams=" + std::to_string(nb_streams()) +
                    " video_stream=" + std::to_string(video_stream_);
    if (codec_ctx_)
        s += " picked_codec=" + std::string(avcodec_get_name(codec_ctx_->codec_id));
    if (fmt_ctx_) {
        int vid = 1;
        for (unsigned i = 0; i < fmt_ctx_->nb_streams; ++i) {
            const AVStream* st = fmt_ctx_->streams[i];
            if (st && st->codecpar && st->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
                s += " V" + std::to_string(vid++) + "=#" + std::to_string(i) + ":";
                s += avcodec_get_name(st->codecpar->codec_id);
                s += ":" + std::to_string(st->codecpar->width) + "x" +
                     std::to_string(st->codecpar->height);
            }
        }
    }
    return s;
}

VideoFramePtr VideoDecoder::decode_forward_to(const int64_t target, const int max_over) {
    if (!codec_ctx_ || frame_rate_ <= 0.0) return nullptr;
    refine_last_frame();
    const auto df_t0 = std::chrono::steady_clock::now();
    int fast_over = 0;
    // Most recently decoded frame, kept so we can fall back to a representative
    // frame if we hit the fast-over cap before reaching target.
    int64_t last_number = -1;
    int64_t last_ticks = AV_NOPTS_VALUE;
    double last_secs = 0.0;
    for (;;) {
        const int ret = avcodec_receive_frame(codec_ctx_, av_frame_);
        if (ret == 0) {
            int64_t ticks = av_frame_->best_effort_timestamp;
            if (ticks == AV_NOPTS_VALUE) ticks = av_frame_->pts;
            const bool have_ts = ticks != AV_NOPTS_VALUE;
            const double secs = have_ts
                ? static_cast<double>(ticks) * av_q2d(stream_tb_)
                : static_cast<double>(next_frame_) / frame_rate_;
            const int64_t number =
                std::max<int64_t>(static_cast<int64_t>(std::llround(secs * frame_rate_)), 0);
            last_number = number; last_ticks = ticks; last_secs = secs;

            if (number < target) {
                // Fast-over this intermediate GOP frame without converting.
                ++fast_over;
                av_frame_unref(av_frame_);
                next_frame_ = number + 1;
                if (max_over > 0 && fast_over >= max_over) {
                    // Too far from the keyframe at preview cost: return the
                    // nearest frame we already have (re-decoding it — the fast-over
                    // unref'd it).
                    auto ap0 = std::chrono::steady_clock::now();
                    if (!draining_) {
                        for (;;) {
                            const int r2 = avcodec_receive_frame(codec_ctx_, av_frame_);
                            if (r2 == 0) break;
                            if (r2 == AVERROR_EOF) { draining_ = true; return nullptr; }
                            if (r2 != AVERROR(EAGAIN)) return nullptr;
                            int pr = -1;
                            for (;;) {
                                pr = av_read_frame(fmt_ctx_, packet_);
                                if (pr == AVERROR_EOF) { draining_ = true; break; }
                                if (pr < 0) break;
                                if (packet_->stream_index != video_stream_) {
                                    av_packet_unref(packet_);
                                    continue;
                                }
                                avcodec_send_packet(codec_ctx_, packet_);
                                av_packet_unref(packet_);
                                break;
                            }
                            if (pr == AVERROR_EOF) { draining_ = true; return nullptr; }
                        }
                    } else {
                        return nullptr;
                    }
                    auto out = make_rgba_frame(av_frame_, last_ticks, last_secs, last_number);
                    av_frame_unref(av_frame_);
                    if (!out) return nullptr;
                    next_frame_ = last_number + 1;
                    const double df_ms = std::chrono::duration<double, std::milli>(
                        std::chrono::steady_clock::now() - df_t0).count();
                    ::canvas::core::log::log_error(
                        "vdecode forward target=%lld got=%lld CAPPED fast_over_frames=%d max=%d ms=%.2f",
                        (long long)target, (long long)last_number, fast_over, max_over, df_ms);
                    return out;
                }
                continue;
            }
            // Target (or next frame at/after it): convert to RGBA.
            auto out = make_rgba_frame(av_frame_, ticks, secs, number);
            av_frame_unref(av_frame_);
            if (!out) continue;
            next_frame_ = number + 1;
            const double df_ms = std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - df_t0).count();
            ::canvas::core::log::log_error(
                "vdecode forward target=%lld got=%lld fast_over_frames=%d ms=%.2f",
                (long long)target, (long long)number, fast_over, df_ms);
            return out;
        }
        if (ret == AVERROR_EOF) {
            if (next_frame_ > 0 && (last_frame_ < 0 || next_frame_ - 1 < last_frame_))
                last_frame_ = next_frame_ - 1;
            CANVAS_LOG("video_decoder: decode_forward_to EOF target=%lld at frame %lld",
                   (long long)target, (long long)next_frame_);
            return nullptr;
        }
        if (ret != AVERROR(EAGAIN)) {
            log::log_error("video_decoder: decode_forward_to ERROR ret=%d target=%lld",
                            ret, (long long)target);
            return nullptr;
        }
        if (draining_) return nullptr;
        for (;;) {
            const int r = av_read_frame(fmt_ctx_, packet_);
            if (r < 0) {
                draining_ = true;
                avcodec_send_packet(codec_ctx_, nullptr);
                break;
            }
            if (packet_->stream_index != video_stream_) {
                av_packet_unref(packet_);
                continue;
            }
            avcodec_send_packet(codec_ctx_, packet_);
            av_packet_unref(packet_);
            break;
        }
    }
}

const AVFrame* VideoDecoder::decode_to_hw(const int64_t target, const int max_over) {
    if (!codec_ctx_ || frame_rate_ <= 0.0) return nullptr;
    refine_last_frame();
    // This path only serves hardware-decoded frames (device NV12 on CUDA); software
    // decode must use the CPU RGBA path. hw_pix_fmt_ is the *hwaccel* format
    // negotiated in open() (AV_PIX_FMT_CUDA) — the tag AVFrame::format carries
    // for device-memory frames. AV_PIX_FMT_NV12 is the nested sw_format inside
    // hw_frames_ctx, never the frame's own format, so comparing against it here
    // used to fail and this function returned null before decoding at all.
    if (!hw_avail_ || hw_pix_fmt_ != AV_PIX_FMT_CUDA) return nullptr;
    CANVAS_LOG("video_decoder: decode_to_hw target=%lld max_over=%d", (long long)target, max_over);
    int fast_over = 0;
    for (;;) {
        const int ret = avcodec_receive_frame(codec_ctx_, av_frame_);
        if (ret == 0) {
            int64_t ticks = av_frame_->best_effort_timestamp;
            if (ticks == AV_NOPTS_VALUE) ticks = av_frame_->pts;
            const bool have_ts = ticks != AV_NOPTS_VALUE;
            const double secs = have_ts
                ? static_cast<double>(ticks) * av_q2d(stream_tb_)
                : static_cast<double>(next_frame_) / frame_rate_;
            const int64_t number =
                std::max<int64_t>(static_cast<int64_t>(std::llround(secs * frame_rate_)), 0);

            if (number < target) {
                // Fast-over this intermediate GPU frame without downloading it. `av_frame_` is
                // a single reused buffer, so count first and only unref if continuing.
                ++fast_over;
                if (max_over > 0 && fast_over >= max_over) {
                    // Too far at preview cost (sparse-keyframe GOP): return the
                    // frame just reached on the device as an approximate teaser;
                    // the next move resumes from here. Borrowed like the exact
                    // target path (caller must consume before the next decode).
                    ::canvas::core::log::log_error(
                        "vdecode hw-forward target=%lld got=%lld CAPPED fast_over_frames=%d max=%d",
                        (long long)target, (long long)number, fast_over, max_over);
                    next_frame_ = number + 1;
                    return av_frame_;
                }
                av_frame_unref(av_frame_);
                next_frame_ = number + 1;
                continue;
            }
            // Target (or next frame at/after it) on the device.
            next_frame_ = number + 1;
            CANVAS_LOG("video_decoder: decode_to_hw OK target=%lld got=%lld",
                   (long long)target, (long long)number);
            return av_frame_;
        }
        if (ret == AVERROR_EOF) {
            if (next_frame_ > 0 && (last_frame_ < 0 || next_frame_ - 1 < last_frame_))
                last_frame_ = next_frame_ - 1;
            CANVAS_LOG("video_decoder: decode_to_hw EOF at frame %lld", (long long)next_frame_);
            return nullptr;
        }
        if (ret != AVERROR(EAGAIN)) {
            log::log_error("video_decoder: decode_to_hw ERROR ret=%d target=%lld",
                            ret, (long long)target);
            return nullptr;
        }
        if (draining_) return nullptr;
        for (;;) {
            const int r = av_read_frame(fmt_ctx_, packet_);
            if (r < 0) {
                draining_ = true;
                avcodec_send_packet(codec_ctx_, nullptr);
                break;
            }
            if (packet_->stream_index != video_stream_) {
                av_packet_unref(packet_);
                continue;
            }
            avcodec_send_packet(codec_ctx_, packet_);
            av_packet_unref(packet_);
            break;
        }
    }
}

const AVFrame* VideoDecoder::decode_to_hw_indexed(const int64_t target, const int max_over) {
    if (!codec_ctx_ || frame_rate_ <= 0.0) return nullptr;
    if (!hw_avail_ || hw_pix_fmt_ != AV_PIX_FMT_CUDA) return nullptr;
    refine_last_frame();
    int64_t t = clamp_target(target);

    // Jump to the keyframe that owns this target so the forward walk only
    // traverses one Group of Pictures. Uses the built keyframe index; without
    // one, a plain container seek to the target's presentation time lands on
    // the keyframe at-or-before, which also anchors backward scrubs.
    const IframeEntry* entry = iframe_at_or_before(t);
    if (entry) {
        container_seek_seconds(entry->pts_seconds);
        ::canvas::core::log::log_error(
            "vdecode hw-indexed target=%lld iframe=%lld gop_secs=%.3f max=%d",
            (long long)t, (long long)entry->frame, entry->pts_seconds, max_over);
    } else if (frame_rate_ > 0.0) {
        container_seek_seconds(static_cast<double>(t) / frame_rate_);
    }
    const AVFrame* hw = decode_to_hw(t, max_over == 0 ? kFullResMaxOver : max_over);  // sets next_frame_ internally
    return hw;
}

VideoFramePtr VideoDecoder::make_rgba_frame(const AVFrame* src, const int64_t ticks,
                                            const double seconds, const int64_t number) {
    auto out = std::make_shared<VideoFrame>();

    // Hardware frames live on the GPU. Pull a CPU-readable copy back (NV12
    // typically) into `sw`, then convert that to RGBA below.
    const AVFrame* cvt = src;
    AVFrame* sw = nullptr;
    if (src->hw_frames_ctx) {
        sw = av_frame_alloc();
        if (!sw || av_hwframe_transfer_data(sw, src, 0) < 0) {
            CANVAS_LOG("decode: hw->cpu transfer FAILED frame=%lld", (long long)number);
            av_frame_free(&sw);
            return nullptr;
        }
        av_frame_copy_props(sw, src);
        cvt = sw;
    }

    // Target output size. With a low-res preview cap set (see set_output_dim),
    // scale during the single sws conversion so scrubbing a GOP doesn't build
    // full-res RGBA for every frame.
    int out_w = cvt->width;
    int out_h = cvt->height;
    if (out_max_dim_ > 0 && out_max_dim_ < std::max(cvt->width, cvt->height)) {
        if (cvt->width >= cvt->height) {
            out_w = out_max_dim_;
            out_h = std::max(1, static_cast<int>(std::llround(
                                    static_cast<double>(cvt->height) * out_max_dim_ / cvt->width)));
        } else {
            out_h = out_max_dim_;
            out_w = std::max(1, static_cast<int>(std::llround(
                                    static_cast<double>(cvt->width) * out_max_dim_ / cvt->height)));
        }
    }

    out->width = out_w;
    out->height = out_h;

    out->stride = static_cast<std::size_t>(out_w) * 4;
    out->pts_ticks = ticks;
    out->pts_seconds = seconds;
    out->frame_number = number;
    out->rgba.resize(out->stride * static_cast<std::size_t>(out_h));

    const bool scaled = (out_w != cvt->width) || (out_h != cvt->height);
    sws_ctx_ = sws_getCachedContext(sws_ctx_, cvt->width, cvt->height,
                                    static_cast<AVPixelFormat>(cvt->format),
                                    out_w, out_h, AV_PIX_FMT_RGBA,
                                    scaled ? SWS_FAST_BILINEAR : SWS_BILINEAR,
                                    nullptr, nullptr, nullptr);
    if (!sws_ctx_) {
        log::log_error("video_decoder: make_rgba sws_getCachedContext FAILED src=%dx%d fmt=%d dst=%dx%d",
                        cvt->width, cvt->height, cvt->format, out_w, out_h);
        av_frame_free(&sw);
        return nullptr;
    }
    uint8_t* dst_data[] = {out->rgba.data()};
    int dst_linesize[] = {static_cast<int>(out->stride)};
    sws_scale(sws_ctx_, cvt->data, cvt->linesize, 0, cvt->height, dst_data, dst_linesize);
    av_frame_free(&sw);
    return out;
}

void VideoDecoder::set_output_dim(int max_output_dim) {
    out_max_dim_ = max_output_dim > 0 ? max_output_dim : 0;
}

VideoFramePtr VideoDecoder::seek_to_frame(int64_t target, int max_output_dim) {
    if (!fmt_ctx_ || frame_rate_ <= 0.0) return nullptr;
    set_output_dim(max_output_dim);
    refine_last_frame();
    target = clamp_target(target);

    const auto us = static_cast<int64_t>(
        std::llround(target / frame_rate_ * static_cast<double>(AV_TIME_BASE)));
    CANVAS_LOG("decode: video seek_to_frame=%lld (%.3fs)", (long long)target,
           target / frame_rate_);
    const auto st_t0 = std::chrono::steady_clock::now();
    if (avformat_seek_file(fmt_ctx_, -1, INT64_MIN, us, us, 0) < 0 && us != 0)
        avformat_seek_file(fmt_ctx_, -1, INT64_MIN, 0, 0, 0);
    reset_stream_state(target);

    VideoFramePtr frame = decode_forward_to(target, kFullResMaxOver);
    const double st_ms = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - st_t0).count();
    ::canvas::core::log::log_error("vdecode container-seek target=%lld ms=%.2f ok=%d",
                               (long long)target, st_ms, frame != nullptr);
    if (!frame) return nullptr;
    next_frame_ = frame->frame_number + 1;
    return frame;
}

VideoFramePtr VideoDecoder::decode_to_frame(int64_t target, int max_output_dim) {
    if (!codec_ctx_ || frame_rate_ <= 0.0) return nullptr;
    set_output_dim(max_output_dim);
    refine_last_frame();
    target = clamp_target(target);

    const auto dbg_start = std::chrono::steady_clock::now();
    const bool dbg_hw = hw_pix_fmt_ != AV_PIX_FMT_NONE;
    VideoFramePtr dbg_out = nullptr;

    // Give sequential decode a small window of forward progress to avoid a
    // random seek on the common playback path. If we've already moved past the
    // target or it's far ahead, seek — a big sequential forward jump would block
    // for (distance * ~ms/frame) and stall scrubbing, while a keyframe seek
    // bounds decode-forward to a single GOP.
    if (target >= next_frame_ && target - next_frame_ < 64) {
VideoFramePtr frame = decode_forward_to(target, kFullResMaxOver);
        if (frame) {
            next_frame_ = frame->frame_number + 1;
            dbg_out = std::move(frame);
        } else {
            // Fall through to a (re)seek if sequential decode stalled.
        }
    }
    if (!dbg_out) dbg_out = seek_to_frame_indexed(target, max_output_dim);

    if (dbg_out) {
        const double dbg_ms = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - dbg_start).count();
        CANVAS_LOG("decode: frame %lld took %.2f ms hw=%d", (long long)dbg_out->frame_number,
               dbg_ms, (int)dbg_hw);
    }
    return dbg_out;
}

// Seek the demuxer to `target_seconds` so a subsequent decode starts there
// (avformat_seek_file lands on the nearest keyframe at-or-before). Only the
// container position is touched; the codec is not flushed here.
void VideoDecoder::container_seek_seconds(const double target_seconds) {
    const auto us = static_cast<int64_t>(
        std::llround(target_seconds * static_cast<double>(AV_TIME_BASE)));
    CANVAS_LOG("video_decoder: container_seek_seconds %.3fs us=%lld", target_seconds, (long long)us);
    if (avformat_seek_file(fmt_ctx_, -1, INT64_MIN, us, us, 0) < 0) {
        CANVAS_LOG("video_decoder: container_seek retry to 0");
        avformat_seek_file(fmt_ctx_, -1, INT64_MIN, 0, 0, 0);
    }
    reset_stream_state(0);
}

// Process-wide I-frame index cache, keyed by media path. Backing storage is
// declared as inline in the header; the builder below populates it on a
// detached thread. Walks the container recording every keyframe, and is safe
// to run concurrently with any decoder.
static std::shared_ptr<const std::vector<IframeEntry>> build_iframe_sync(const std::string& path,
                                                                         const AVRational stream_tb,
                                                                         const double fps,
                                                                         const int vstream) {
    auto out = std::make_shared<std::vector<IframeEntry>>();
    AVFormatContext* ctx = nullptr;
    if (avformat_open_input(&ctx, path.c_str(), nullptr, nullptr) < 0) return nullptr;
    if (avformat_find_stream_info(ctx, nullptr) < 0) {
        avformat_close_input(&ctx);
        return out;
    }
    AVPacket* pkt = av_packet_alloc();
    if (!pkt) {
        avformat_close_input(&ctx);
        return out;
    }
    for (;;) {
        const int r = av_read_frame(ctx, pkt);
        if (r < 0) break;
        if (pkt->stream_index == vstream && (pkt->flags & AV_PKT_FLAG_KEY)) {
            double secs = 0.0;
            const int64_t ticks = pkt->dts != AV_NOPTS_VALUE ? pkt->dts : pkt->pts;
            if (ticks != AV_NOPTS_VALUE) secs = static_cast<double>(ticks) * av_q2d(stream_tb);
            const int64_t frame = fps > 0.0
                ? std::max<int64_t>(static_cast<int64_t>(std::llround(secs * fps)), 0)
                : -1;
            if (frame >= 0) out->push_back({pkt->pos, secs, frame});
        }
        av_packet_unref(pkt);
    }
    av_packet_free(&pkt);
    avformat_close_input(&ctx);
    return out;
}

// Walks the container packet stream in the background recording every
// keyframe. Callers never block: seeks fall back to plain container access
// until the index is ready.
void VideoDecoder::build_iframe_index() {
    const std::string path = path_;
    if (path.empty() || video_stream_ < 0 || frame_rate_ <= 0.0) return;

    const AVRational stream_tb = stream_tb_;
    const double fps = frame_rate_;
    const int vstream = video_stream_;

    {
        std::lock_guard<std::mutex> lk(s_iframe_mtx);
        if (s_iframe_cache.count(path) || !s_iframe_inflight.insert(path).second) return;
    }
    std::thread([path, stream_tb, fps, vstream] {
        auto built = build_iframe_sync(path, stream_tb, fps, vstream);
        std::lock_guard<std::mutex> lk2(s_iframe_mtx);
        if (built && built->size() > 1 && !s_iframe_cache.count(path))
            s_iframe_cache[path] = std::move(built);
        s_iframe_inflight.erase(path);
        CANVAS_LOG("video_decoder: iframe_index built for '%s' entries=%zu cached=%d",
               path.c_str(), built ? built->size() : 0,
               s_iframe_cache.count(path) > 0);
    }).detach();
}

const IframeEntry* VideoDecoder::iframe_at_or_before(const int64_t target) const {
    if (target < 0 || path_.empty()) return nullptr;
    std::lock_guard<std::mutex> lk(s_iframe_mtx);
    auto it = s_iframe_cache.find(path_);
    if (it == s_iframe_cache.end()) return nullptr;
    const IframeEntry* best = nullptr;
    for (const IframeEntry& e : *it->second) {
        if (e.frame <= target) best = &e;
        else break;
    }
    return best;
}

VideoFramePtr VideoDecoder::seek_to_frame_indexed(int64_t target, int max_output_dim) {
    set_output_dim(max_output_dim);
    refine_last_frame();
    target = clamp_target(target);

    // Find the I-frame that owns this target (at-or-before).
    const IframeEntry* entry = iframe_at_or_before(target);
    if (!entry) {
        CANVAS_LOG("video_decoder: seek_to_frame_indexed NO IFRAME for %lld, fallback to container seek",
               (long long)target);
        return seek_to_frame(target, max_output_dim);
    }

    // Jump to the keyframe's presentation time, decode-forward to `target` within
    // this single GOP, and re-sync state.
    // Preview path (max_output_dim > 0) caps decode-forward so a scrub never
    // stalls on sparse-keyframe GOPs: it decodes at most ~kPreviewMaxOver frames
    // past the keyframe and returns the nearest frame reached (an approximate
    // low-res tease) instead of walking the entire multi-second GOP. Any other
    // dim (0 = full-res, or a stray negative) walks at most ~kFullResMaxOver
    // frames for the same reason.
    //
    // BUT a full-res far-forward seek into an ultra-sparse GOP (observed: a
    // 19,000-frame keyframe interval) used to walk THE WHOLE GOP uncapped — one
    // scrub position took 78 s of decode-forward, during which audio pre-rolled
    // ~6 s ahead and the device drained, garbling the first seconds of playback.
    // A NEITHER-positive-nor-zero max_output_dim previously fell through to
    // max_over=0 (uncapped again; field log: 51-58s scrub walks in a 8+ min AV1
    // with iframe=16000). Cap those too, so no caller can ever re-enable the
    // unbounded walk: a far seek returns the nearest decoded frame as an
    // approximate still instead of blocking the worker for tens of seconds.
    const int max_over = max_output_dim > 0 ? kPreviewMaxOver : kFullResMaxOver;
    const auto idx_t0 = std::chrono::steady_clock::now();
    container_seek_seconds(entry->pts_seconds);
    VideoFramePtr frame = decode_forward_to(target, max_over);
    const double idx_ms = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - idx_t0).count();
    ::canvas::core::log::log_error("vdecode scrub indexed-seek target=%lld iframe=%lld gop_secs=%.3f max=%d ms=%.2f ok=%d",
                               (long long)target, (long long)entry->frame,
                               entry->pts_seconds, max_over, idx_ms, frame != nullptr);
    if (!frame) return nullptr;
    next_frame_ = frame->frame_number + 1;
    return frame;
}

void VideoDecoder::seek_audio(const int64_t start_sample, const int sample_rate) {
    if (!audio_fmt_ctx_ || audio_stream_ < 0) return;
    int64_t us = 0;
    if (sample_rate > 0)
        us = static_cast<int64_t>(
            std::llround(start_sample / static_cast<double>(sample_rate) * AV_TIME_BASE));
    if (avformat_seek_file(audio_fmt_ctx_, audio_stream_, INT64_MIN, us, us, 0) < 0 && us != 0)
        avformat_seek_file(audio_fmt_ctx_, -1, INT64_MIN, 0, 0, 0);
    if (audio_codec_) avcodec_flush_buffers(audio_codec_);
    if (swr_ctx_) {
        swr_free(&swr_ctx_);
        swr_ctx_ = nullptr;
    }
    audio_next_sample_ = start_sample;
    CANVAS_LOG("decode: audio seek to sample=%lld (%.3fs)", (long long)start_sample,
           start_sample / (sample_rate > 0 ? static_cast<double>(sample_rate) : 1.0));
}

namespace {

bool ensure_audio_swr(SwrContext*& swr, const AVCodecContext* c, const int out_rate,
                      const int channels) {
    if (swr) return true;
    AVChannelLayout out_layout;
    av_channel_layout_default(&out_layout, channels);
    SwrContext* s = nullptr;
    const int alloc =
        swr_alloc_set_opts2(&s, &out_layout, AV_SAMPLE_FMT_FLT, out_rate, &c->ch_layout,
                            c->sample_fmt, c->sample_rate, 0, nullptr);
    av_channel_layout_uninit(&out_layout);
    if (alloc < 0 || !s || swr_init(s) < 0) {
        if (s) swr_free(&s);
        ::canvas::core::log::log_error(
            "video_decoder: ensure_audio_swr FAILED alloc=%d swr=%p in_rate=%d out_rate=%d in_ch=%d out_ch=%d",
            alloc, (void*)s, c->sample_rate, out_rate, c->ch_layout.nb_channels, channels);
        return false;
    }
    CANVAS_LOG("video_decoder: ensure_audio_swr OK in_rate=%d out_rate=%d in_ch=%d out_ch=%d",
           c->sample_rate, out_rate, c->ch_layout.nb_channels, channels);
    swr = s;
    return true;
}

}  // namespace

// Decodes interleaved float PCM at `out_sample_rate`. `start_sample` is in
// output sample units (i.e. seconds * out_sample_rate). Each resampled block
// is positioned by its source frame's best_effort_timestamp mapped into
// output-sample units, so positioning stays exact regardless of seek history or
// resampler delay. Sequential decode is used when the caller advances
// monotonically; backward/far-forward targets trigger a reseek and any samples
// preceding the target are discarded.
AudioChunkPtr VideoDecoder::decode_audio(const int64_t start_sample, const int max_frames,
                                         const int out_sample_rate) {
    if (!audio_codec_ || !audio_fmt_ctx_ || audio_stream_ < 0 || out_sample_rate <= 0 ||
        max_frames <= 0) {
        CANVAS_LOG("video_decoder: decode_audio SKIP codec=%p fmt=%p stream=%d rate=%d max=%d",
               (void*)audio_codec_, (void*)audio_fmt_ctx_, audio_stream_,
               out_sample_rate, max_frames);
        return nullptr;
    }
    const int64_t target = std::max<int64_t>(0, start_sample);

    // Reseek only on backward jumps or large forward jumps; sequential calls
    // (the common playback path) continue decoding and discard passed samples.
    if (target < audio_next_sample_ ||
        target - audio_next_sample_ > 2 * static_cast<int64_t>(out_sample_rate)) {
        CANVAS_LOG("decode: audio reseek target=%lld cur=%lld (rate=%d)",
               (long long)target, (long long)audio_next_sample_, out_sample_rate);
        seek_audio(target, out_sample_rate);
    }
    if (!ensure_audio_swr(swr_ctx_, audio_codec_, out_sample_rate, audio_channels_)) {
        log::log_error("video_decoder: decode_audio ensure_swr FAILED target=%lld", (long long)target);
        return nullptr;
    }

    const int ch = audio_channels_;
    std::vector<float> out;
    out.reserve(static_cast<std::size_t>(max_frames) * ch);
    int64_t produced = 0;
    const int64_t end = target + max_frames;

    int ret = AVERROR(EAGAIN);
    while (produced < max_frames) {
        if (ret == 0 || ret == AVERROR(EAGAIN)) {
            ret = avcodec_receive_frame(audio_codec_, audio_frame_);
        }
        if (ret == 0) {
            const int in_samples = audio_frame_->nb_samples;
            // Position (output-sample units) at the start of this decoded frame.
            int64_t ticks = audio_frame_->best_effort_timestamp;
            if (ticks == AV_NOPTS_VALUE) ticks = audio_frame_->pts;
            const double frame_secs =
                ticks != AV_NOPTS_VALUE && audio_tb_.den != 0
                    ? static_cast<double>(ticks) * av_q2d(audio_tb_)
                    : (audio_next_sample_ / static_cast<double>(out_sample_rate));
            const int64_t block_start =
                static_cast<int64_t>(std::llround(frame_secs * out_sample_rate));

            if (in_samples > 0 && swr_ctx_) {
                const int max_out = static_cast<int>(swr_get_out_samples(swr_ctx_, in_samples));
                std::vector<float> tmp(static_cast<std::size_t>(max_out) * ch);
                uint8_t* tmp_ptr[1] = {reinterpret_cast<uint8_t*>(tmp.data())};
                const int got =
                    swr_convert(swr_ctx_, tmp_ptr, max_out,
                                const_cast<const uint8_t**>(audio_frame_->extended_data),
                                in_samples);
                if (got > 0) {
                    const int64_t from = std::max<int64_t>(0, target - block_start);
                    const int64_t to = std::min<int64_t>(
                        static_cast<int64_t>(got), std::min<int64_t>(end - block_start, max_frames - produced + from));
                    if (to > from) {
                        out.insert(out.end(), tmp.begin() + from * ch, tmp.begin() + to * ch);
                        produced += to - from;
                    }
                    audio_next_sample_ = block_start + got;
                }
            }
            av_frame_unref(audio_frame_);
            if (produced >= max_frames) break;
            continue;
        }
        if (ret == AVERROR_EOF) {
            CANVAS_LOG("video_decoder: decode_audio EOF at target=%lld produced=%lld",
                   (long long)target, (long long)produced);
            break;
        }
        if (ret != AVERROR(EAGAIN)) {
            log::log_error("video_decoder: decode_audio ERROR ret=%d target=%lld",
                            ret, (long long)target);
            break;
        }

        // Need more packets: read the next audio packet.
        for (;;) {
            const int r = av_read_frame(audio_fmt_ctx_, audio_packet_);
            if (r < 0) {
                avcodec_send_packet(audio_codec_, nullptr);
                break;
            }
            if (audio_packet_->stream_index != audio_stream_) {
                av_packet_unref(audio_packet_);
                continue;
            }
            avcodec_send_packet(audio_codec_, audio_packet_);
            av_packet_unref(audio_packet_);
            break;
        }
        ret = AVERROR(EAGAIN);
    }

    if (produced <= 0) {
        CANVAS_LOG("video_decoder: decode_audio EMPTY target=%lld produced=%lld",
               (long long)target, (long long)produced);
        return nullptr;
    }
    CANVAS_LOG("decode: audio chunk target=%lld produced=%lld frames", (long long)target,
           (long long)produced);
    auto chunk = std::make_shared<AudioChunk>();
    chunk->start_sample = target;
    chunk->sample_rate = out_sample_rate;
    chunk->channels = ch;
    chunk->samples = std::move(out);
    return chunk;
}

}
