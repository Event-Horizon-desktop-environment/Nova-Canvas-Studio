#include "canvas/core/media/video_decoder.hpp"
#include "canvas/core/util/color_log.hpp"
#include "canvas/core/util/log.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <sys/stat.h>
#include <thread>
#include <tuple>

extern "C" {
#include <libavutil/pixdesc.h>
}

namespace canvas::core {

namespace {

// Read-stall detector (~1/s): counts av_read_frame calls that blew past the
// 20ms "smooth demux" bound. A spiky stall_ms while decode ms stays flat is the
// solvent wrapper around slow/disconnected storage; it lights up long before
// the decode-time aggregates do.
void read_stall_tick(const double ms) {
    static auto s_at = std::chrono::steady_clock::now();
    static int s_total = 0, s_stalls = 0;
    static double s_over_ms = 0.0, s_max = 0.0;
    ++s_total;
    if (ms >= 20.0) {
        ++s_stalls;
        s_over_ms += ms - 20.0;
        s_max = std::max(s_max, ms);
    }
    const auto now = std::chrono::steady_clock::now();
    if (s_total == 1 || now - s_at >= std::chrono::seconds(1)) {
        s_at = now;
        if (s_stalls > 0)
            ::canvas::core::log::log_warning(
                "[io] reads=%d stalls_ge20ms=%d over_ms=%.0f max_ms=%.0f",
                s_total, s_stalls, s_over_ms, s_max);
        s_total = 0;
        s_stalls = 0;
        s_over_ms = 0.0;
        s_max = 0.0;
    }
}

// Consecutive-decode-failure burst tracker. Three errors in a row across the
// walk loops is the signature of a dropped NVDEC session, driver reset, or a
// corrupt media tail — one warning at the crossing, reset on any success.
void decode_fail(const char* where) {
    static int burst = 0;
    if (++burst == 3)
        ::canvas::core::log::log_warning("[dec] fail_burst=%d where=%s", burst, where);
}
void decode_ok() {
    static int burst = 0;
    burst = 0;
}

// Compact single-token rendering of an AVCodecParameters color trio for the
// always-on [dec] open line. These are the raw tags the SOURCE file declares
// (range/matrix/trc within its container); everything downstream in the app
// currently assumes a fixed "bt709 limited" regardless of what these say, so
// this line is the ground truth a log-trace is compared against.
const char* color_tag_str(int c) {
    switch (c) {
        case AVCOL_RANGE_MPEG: return "mpeg";
        case AVCOL_RANGE_JPEG: return "jpeg";
        case AVCOL_RANGE_UNSPECIFIED: return "unspec";
        default: return "?";
    }
}
const char* matrix_tag_str(int c) {
    switch (c) {
        case AVCOL_SPC_BT709: return "bt709";
        case AVCOL_SPC_BT470BG: return "bt601";
        case AVCOL_SPC_SMPTE170M: return "smpte170m";
        case AVCOL_SPC_BT2020_NCL: return "bt2020ncl";
        case AVCOL_SPC_RGB: return "rgb";
        case AVCOL_SPC_UNSPECIFIED: return "unspec";
        default: return "?";
    }
}
const char* trc_tag_str(int c) {
    switch (c) {
        case AVCOL_TRC_BT709: return "bt709";
        case AVCOL_TRC_GAMMA22: return "gamma22";
        case AVCOL_TRC_SMPTEST2084: return "pq";
        case AVCOL_TRC_UNSPECIFIED: return "unspec";
        case AVCOL_TRC_RESERVED0: return "res0";
        default: return "?";
    }
}

// ---------------------------------------------------------------------------
// Full-range liar-tag detection (OBS-family recorders).
// OBS can stamp `color_range=tv` while writing FULL-range (0..255) data.
// Trusting that tag over-scales every frame — blowing out highlights, lifting
// blacks, over-saturating chroma; the lavender thread. A limited-range encode
// can never produce luma below ~6 or above ~246 (it floors at 16 and caps at
// 235), so decoding frames on a throwaway software context and watching RAW
// luma separates the two: seeing such an extreme upgrades the range to full.
//
// The probe samples SEVERAL positions across the file (~2 frames at ~7 spots),
// not just the head — a file can keep its full-white / full-black evidence
// until late in the reel (both recordings this hunts carry YMAX=255 / YMIN<8
// only in the final minutes), so a head-only scan wrongly keeps `tv`. Each
// position decodes at a stride of 4 px/row: ~1/16 of the pixels, more than
// enough to catch any out-of-limited-range sample. The verdict is cached per
// (path, size, mtime) so the per-second thumbnail-service decoder reopens
// don't re-run the probe.
std::mutex g_range_mu;
std::map<std::string, gpu::ColorRange> g_range_cache;

namespace {

// Scans one decoded frame's luma plane at stride 4. Returns true as soon as a
// sample sits outside the limited-range envelope (<=6 or >=246).
bool frame_shows_full_range(const AVFrame* fr, int& min_y, int& max_y) {
    if (!fr->data[0]) return false;
    for (int r = 0; r < fr->height; r += 4) {
        const uint8_t* row = fr->data[0] + std::size_t(r) * fr->linesize[0];
        for (int c = 0; c < fr->width; c += 4) {
            const int v = row[c];
            if (v < min_y) min_y = v;
            if (v > max_y) max_y = v;
        }
    }
    return min_y <= 6 || max_y >= 246;
}

}  // namespace

gpu::ColorRange probe_file_range(const std::string& path) {
    AVFormatContext* fc = nullptr;
    AVCodecContext* cc = nullptr;
    AVPacket* pkt = nullptr;
    int min_y = 256, max_y = -1;
    if (avformat_open_input(&fc, path.c_str(), nullptr, nullptr) < 0) return gpu::ColorRange::Limited;
    if (avformat_find_stream_info(fc, nullptr) < 0) {
        avformat_close_input(&fc);
        return gpu::ColorRange::Limited;
    }
    const int vs = av_find_best_stream(fc, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (vs >= 0) {
        const AVCodec* codec = avcodec_find_decoder(fc->streams[vs]->codecpar->codec_id);
        if (codec) {
            cc = avcodec_alloc_context3(codec);
            const AVPixFmtDescriptor* desc = nullptr;
            if (cc && avcodec_parameters_to_context(cc, fc->streams[vs]->codecpar) >= 0) {
                desc = av_pix_fmt_desc_get(cc->pix_fmt);
            }
            // Only probe formats whose first plane is 8-bit luma; 10-bit or
            // packed sources keep the tag verdict (their tags are reliable).
            if (desc && desc->comp[0].depth == 8 &&
                !(desc->flags & AV_PIX_FMT_FLAG_HWACCEL)) {
                if (avcodec_open2(cc, codec, nullptr) >= 0) {
                    pkt = av_packet_alloc();
// Sample positions across the reel (head, quarters, tail).
                    // Note: fc->duration is in AV_TIME_BASE units (microseconds);
                    // the stream's own duration is in stream time_base. Mixing
                    // them up overshoots the seek targets by ~1000x and lands at
                    // EOF, so the probe decodes nothing. Prefer the stream
                    // duration, fall back to fc->duration scaled by AV_TIME_BASE_Q.
                    const int64_t dur_fmt = fc->duration;
                    const int64_t dur_str = fc->streams[vs]->duration;
                    const double sec_base =
                        dur_str > 0
                            ? static_cast<double>(dur_str) *
                                  av_q2d(fc->streams[vs]->time_base)
                            : dur_fmt > 0 ? static_cast<double>(dur_fmt) * av_q2d(AV_TIME_BASE_Q)
                                          : 60.0;
                    // 0.0 first: capture the reel head, where capture prefixes
                    // and OBS color "flash" the most (the old head-only probe
                    // caught these; a pure mid-reel sweep misses them).
                    static const double kFractions[] = {0.00, 0.10, 0.20, 0.35, 0.50,
                                                        0.65, 0.80, 0.90, 0.95};
                    for (const double frac : kFractions) {
                        if (min_y <= 6 || max_y >= 246) break;
                        // The 0.0 position walks sequentially from the reel head
                        // (no seek): a capture-header flash lands a few frames
                        // into the opening GOP, and a seek decoding only
                        // immediately after a keyframe would miss it. Mid-reel
                        // positions seek, then decode the frames after the
                        // backward keyframe.
                        if (frac > 0.0) {
                            const double target_s = sec_base * frac;
                            if (av_seek_frame(fc, vs,
                                              static_cast<int64_t>(target_s / av_q2d(fc->streams[vs]->time_base)),
                                              AVSEEK_FLAG_BACKWARD) < 0)
                                continue;
                            // Reset the decoder for the new position. (avcodec_
                            // send_packet(cc, nullptr) + drain would leave the
                            // codec in EOF state and every later send_packet would
                            // return AVERROR_EOF, silently skipping the position.)
                            avcodec_flush_buffers(cc);
                        }
                        const int kFrames = frac > 0.0 ? 3 : 8;
                        // Decode up to kFrames VIDEO frames at this position (0
                        // tolerance for a seek landing on an unusable frame).
                        // The budget counts video frames, not packets: MKV
                        // interleaves audio between video blocks, so a packet
                        // budget would drain on audio and miss a head flash.
                        int got = 0;
                        while (got < kFrames && av_read_frame(fc, pkt) >= 0) {
                            if (pkt->stream_index != vs) {
                                av_packet_unref(pkt);
                                continue;
                            }
                            const int send = avcodec_send_packet(cc, pkt);
                            av_packet_unref(pkt);
                            if (send < 0) break;
                            bool fired = false;
                            for (;;) {
                                AVFrame* fr = av_frame_alloc();
                                if (!fr) break;
                                const int ret = avcodec_receive_frame(cc, fr);
                                if (ret < 0) {
                                    av_frame_free(&fr);
                                    break;
                                }
                                fired = frame_shows_full_range(fr, min_y, max_y);
                                av_frame_free(&fr);
                                if (fired) break;
                            }
                            ++got;
                            if (fired || min_y <= 6 || max_y >= 246) break;
                        }
                    }
                    av_packet_free(&pkt);
                }
            }
            if (cc) avcodec_free_context(&cc);
        }
    }
    avformat_close_input(&fc);
    return (min_y <= 6 || max_y >= 246) ? gpu::ColorRange::Full : gpu::ColorRange::Limited;
}

gpu::ColorRange cached_color_range_probe(const std::string& path) {
    struct stat st;
    if (::stat(path.c_str(), &st) != 0) return gpu::ColorRange::Limited;
    const std::string key = path + '#' + std::to_string(static_cast<long long>(st.st_size)) +
                            '#' + std::to_string(static_cast<long long>(st.st_mtime));
    {
        std::lock_guard<std::mutex> lk(g_range_mu);
        const auto it = g_range_cache.find(key);
        if (it != g_range_cache.end()) return it->second;
    }
    const gpu::ColorRange range = probe_file_range(path);
    {
        std::lock_guard<std::mutex> lk(g_range_mu);
        g_range_cache.emplace(key, range);
    }
    return range;
}

}  // namespace

VideoDecoder::~VideoDecoder() { close(); }

bool VideoDecoder::open(const std::string& path, std::string* error,
                        const AVBufferRef* hw_device_ctx) {
    close();
    path_ = path;
    const auto open_t0 = std::chrono::steady_clock::now();

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

    // --- Resolve the per-file color spec from the codecpar tags ------------
    // matrix: the color_space tag, falling back to color_primaries when missing.
    // range:  the color_range tag, then a luma probe upgrades Limited->Full for
    // OBS-family files that stamp `tv` while writing full-range data.
    switch (stream->codecpar->color_space) {
        case AVCOL_SPC_BT470BG:
        case AVCOL_SPC_SMPTE170M:
            matrix_ = gpu::ColorMatrix::BT601;
            break;
        case AVCOL_SPC_BT2020_NCL:
        case AVCOL_SPC_BT2020_CL:
            matrix_ = gpu::ColorMatrix::BT2020;
            break;
        default:
            matrix_ = gpu::ColorMatrix::BT709;
            break;
    }
    if (stream->codecpar->color_space == AVCOL_SPC_UNSPECIFIED) {
        switch (stream->codecpar->color_primaries) {
            case AVCOL_PRI_BT470BG:
            case AVCOL_PRI_SMPTE170M:
                matrix_ = gpu::ColorMatrix::BT601;
                break;
            case AVCOL_PRI_BT2020:
                matrix_ = gpu::ColorMatrix::BT2020;
                break;
            default:
                break;
        }
    }
    range_ = (stream->codecpar->color_range == AVCOL_RANGE_JPEG) ? gpu::ColorRange::Full
                                                                 : gpu::ColorRange::Limited;
    const char* range_origin = "tags";
    if (range_ == gpu::ColorRange::Limited) {
        const gpu::ColorRange probed = cached_color_range_probe(path);
        if (probed == gpu::ColorRange::Full) {
            range_ = gpu::ColorRange::Full;
            range_origin = "probe";
        }
    }

    // Always-on: per-media open cost. Media that takes seconds to open (giant
    // mp4 index, slow disk, network mount) is the #1 "paused scrub hangs when it
    // first touches a clip" cause; the [dec] ms number below is whole-file.
    // The color trio is what the SOURCE declares (range/matrix/trc) — the app
    // currently assumes fixed bt709/limited everywhere regardless, so trace
    // this line against the shader/swscale assumptions when chasing hue errors.
    const double open_ms = std::chrono::duration<double, std::milli>(
                               std::chrono::steady_clock::now() - open_t0).count();
    ::canvas::core::log::log_warning(
        "[dec] open media=%s ms=%.0f dims=%dx%d fps=%.3f frames=%lld hw=%s audio=%s "
        "tags=range:%s/matrix:%s/trc:%s",
        path.c_str(), open_ms, width_, height_, frame_rate_,
        static_cast<long long>(total_frames_), hw_avail_ ? "hw" : "sw",
        audio_stream_ >= 0 ? "yes" : "no",
        color_tag_str(stream->codecpar->color_range),
        matrix_tag_str(stream->codecpar->color_space),
        trc_tag_str(stream->codecpar->color_trc));
    log::log_warning(
        "[dec] color spec resolved: matrix=%s range=%s (%s)",
        gpu::color_matrix_name(matrix_), gpu::color_range_name(range_), range_origin);

    // Always-on color archive (color.log, no gate, not canvas_debug.log): the
    // per-media baseline gathered BEFORE any grading — what the source
    // declared versus what the probe resolved, so a later wheel/curve change
    // can be judged against the frame's actual color spec.
    CANVAS_COLOR_LOG(
        "[media] open path=%s codec=%s dims=%dx%d tags=range:%s/matrix:%s/trc:%s "
        "resolved=matrix:%s/range:%s verdict=%s",
        path.c_str(), avcodec_get_name(stream->codecpar->codec_id), width_, height_,
        color_tag_str(stream->codecpar->color_range),
        matrix_tag_str(stream->codecpar->color_space),
        trc_tag_str(stream->codecpar->color_trc), gpu::color_matrix_name(matrix_),
        gpu::color_range_name(range_), range_origin);

    next_frame_ = 0;
    draining_ = false;
    hw_engaged_ = false;
    soft_only_ = false;
    return true;
}

void VideoDecoder::close() {
    CANVAS_LOG("video_decoder: close '%s' hw=%d next_frame=%lld",
           path_.c_str(), (int)hw_avail_, (long long)next_frame_);
    if (packet_) av_packet_free(&packet_);
    if (av_frame_) av_frame_free(&av_frame_);
    if (hold_hw_) av_frame_free(&hold_hw_);
    hold_hw_src_ = -1;
    hold_rgba_.reset();
    hold_rgba_src_ = -1;
    hold_rgba_dim_ = -1;
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
    hw_engaged_ = false;
    soft_only_ = false;
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
            decode_ok();
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
            decode_fail("decode_next");
            return nullptr;
        }
        if (draining_) return nullptr;
        for (;;) {
            const auto rd_t0 = std::chrono::steady_clock::now();
            const int r = av_read_frame(fmt_ctx_, packet_);
            read_stall_tick(std::chrono::duration<double, std::milli>(
                                std::chrono::steady_clock::now() - rd_t0).count());
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
                                const auto rdr_t0 = std::chrono::steady_clock::now();
                                pr = av_read_frame(fmt_ctx_, packet_);
                                read_stall_tick(std::chrono::duration<double, std::milli>(
                                                    std::chrono::steady_clock::now() - rdr_t0).count());
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
CANVAS_LOG(
                    "vdecode forward target=%lld got=%lld CAPPED fast_over_frames=%d max=%d ms=%.2f",
                    (long long)target, (long long)last_number, fast_over, max_over, df_ms);
                    decode_ok();
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
            CANVAS_LOG(
                "vdecode forward target=%lld got=%lld fast_over_frames=%d ms=%.2f",
                (long long)target, (long long)number, fast_over, df_ms);
            decode_ok();
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
            decode_fail("decode_forward_to");
            return nullptr;
        }
        if (draining_) return nullptr;
        for (;;) {
            const auto rd_t0 = std::chrono::steady_clock::now();
            const int r = av_read_frame(fmt_ctx_, packet_);
            read_stall_tick(std::chrono::duration<double, std::milli>(
                                std::chrono::steady_clock::now() - rd_t0).count());
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

    // Frozen-tail hold: once the stream's final frame is known (last_frame_),
    // a request past it must not re-walk to EOF and fail on every call — the
    // exporter then silently drops to per-frame CPU re-seeks (~90ms/frame) for
    // the whole audio-only tail. Serve a persistent ref of the last real frame
    // instead, built once by re-anchoring on the owning keyframe.
    if (last_frame_ > 0 && target > last_frame_) {
        if (!hold_hw_ || hold_hw_src_ != last_frame_) {
            const IframeEntry* entry = iframe_at_or_before(last_frame_);
            if (entry) {
                container_seek_seconds(entry->pts_seconds);
            } else if (frame_rate_ > 0.0) {
                container_seek_seconds(static_cast<double>(last_frame_) / frame_rate_);
            }
            const AVFrame* last = decode_to_hw(last_frame_, 0);
            if (last && last->format == hw_pix_fmt_) {
                if (!hold_hw_) hold_hw_ = av_frame_alloc();
                if (hold_hw_ && av_frame_ref(hold_hw_, last) == 0) {
                    hold_hw_src_ = last_frame_;
                } else {
                    av_frame_free(&hold_hw_);
                    hold_hw_src_ = -1;
                }
            } else {
                av_frame_free(&hold_hw_);
                hold_hw_src_ = -1;
            }
        }
        return hold_hw_;  // nullptr on failure -> caller falls back to CPU RGBA
    }
    CANVAS_LOG("video_decoder: decode_to_hw target=%lld max_over=%d", (long long)target, max_over);
    // Far-jump re-anchor, mirroring decode_to_frame's sequential-vs-seek rule.
    // A fresh decoder starts at the container head, so a deep target reached by
    // fast-overing every frame is a multi-second stall (seen in GPU exports that
    // re-open after a clip change). Seek to the owning keyframe instead so the
    // forward walk spans a single GOP. The common +1 sequential step never
    // crosses the 64-frame cutoff and stays on the walk path.
    //
    // Bounds the check on BOTH sides of the stream position: re-anchoring only
    // on `target < next_frame_` would re-fire on a plain 1-frame timestamp
    // overshoot (the fast-over walk returns the frame just past target, so
    // next_frame_ legitimately reads target+2 and the very next +1 request
    // looks "behind"), turning a sequential GPU export into a per-GOP re-seek.
    // A seek is only worthwhile when the requested position is far from where
    // the walk currently sits; small overshoots are absorbed by walking.
    const int64_t delta = target - next_frame_;
    if (delta >= 64 || -delta >= 64) {
        const int64_t prev_next = next_frame_;
        const IframeEntry* entry = iframe_at_or_before(target);
        if (entry) {
            container_seek_seconds(entry->pts_seconds);
        } else if (frame_rate_ > 0.0 && target > 0) {
            container_seek_seconds(static_cast<double>(target) / frame_rate_);
        }
        log::log_warning("[dec] hw far-jump target=%lld next=%lld delta=%lld iframe=%lld",
                         (long long)target, (long long)prev_next, (long long)delta,
                         entry ? (long long)entry->frame : -1LL);
    }
    // A CUDA decode session is primed by the bitstream's owning keyframe (the
    // AV1 sequence header). Entering mid-GOP makes FFmpeg transparently emit
    // SOFTWARE frames from a hardware-configured decoder; the NV12 composite
    // kernel then fails and the caller drops to the full-res CPU RGBA path for
    // the whole stretch (~6-15fps on 2K60 instead of 30). Detect the software
    // emission once and re-anchor on the owning keyframe so NVDEC actually
    // engages; once engaged, every later seek keeps producing device frames.
    int fast_over = 0;
    for (int attempt = 0; attempt < 2; ++attempt) {
        if (attempt == 1) {
            if (soft_only_) break;
            const IframeEntry* entry = iframe_at_or_before(target);
            if (entry) {
                container_seek_seconds(entry->pts_seconds);
                ::canvas::core::log::log_warning(
                    "[dec] HW-ENGAGE target=%lld iframe=%lld gop_secs=%.3f",
                    (long long)target, (long long)entry->frame, entry->pts_seconds);
            } else if (frame_rate_ > 0.0 && target > 0) {
                container_seek_seconds(static_cast<double>(target) / frame_rate_);
            } else {
                break;
            }
            fast_over = 0;
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

                if (number < target) {
                    // Fast-over this intermediate GPU frame without downloading it. `av_frame_` is
                    // a single reused buffer, so count first and only unref if continuing.
                    ++fast_over;
                    if (max_over > 0 && fast_over >= max_over) {
                        // Too far at preview cost (sparse-keyframe GOP): return the
                        // frame just reached on the device as an approximate teaser;
                        // the next move resumes from here. Borrowed like the exact
                        // target path (caller must consume before the next decode).
                        CANVAS_LOG(
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
                if (av_frame_->format != hw_pix_fmt_) {
                    // SOFTWARE frame from a hardware-configured decoder (mid-GOP
                    // entry; the owning keyframe never primed the CUDA session). On
                    // the first attempt re-anchor so NVDEC engages; if the retry
                    // still yields software, latch soft_only_ and hand it back — the
                    // caller's GPU composite will fail and fall back to CPU RGBA,
                    // which is exactly the behavior this re-anchor fixes.
                    if (attempt == 0 && !hw_engaged_) {
                        av_frame_unref(av_frame_);
                        CANVAS_LOG("video_decoder: hw emitting software target=%lld got=%lld; "
                                   "re-anchoring keyframe",
                                   (long long)target, (long long)number);
                        break;
                    }
                    if (attempt == 1) soft_only_ = true;
                    return av_frame_;
                }
                hw_engaged_ = true;
                CANVAS_LOG("video_decoder: decode_to_hw OK target=%lld got=%lld",
                       (long long)target, (long long)number);
                decode_ok();
                return av_frame_;
            }
            if (ret == AVERROR_EOF) {
                if (next_frame_ > 0 && (last_frame_ < 0 || next_frame_ - 1 < last_frame_)) {
                    const int64_t old_last = last_frame_;
                    last_frame_ = next_frame_ - 1;
                    log::log_warning(
                        "[dec] HW-EOF srct=%-9lld hit=%-9lld last %-9lld -> %-9lld "
                        "engaged=%d soft_only=%d hw=%d fmt=%d (latched, no re-anchor)",
                        (long long)target, (long long)(next_frame_ - 1),
                        (long long)old_last, (long long)last_frame_,
                        hw_engaged_ ? 1 : 0, soft_only_ ? 1 : 0, hw_avail_ ? 1 : 0, hw_pix_fmt_);
                }
                CANVAS_LOG("video_decoder: decode_to_hw EOF at frame %lld", (long long)next_frame_);
                return nullptr;
            }
            if (ret != AVERROR(EAGAIN)) {
                log::log_error("video_decoder: decode_to_hw ERROR ret=%d target=%lld",
                                ret, (long long)target);
                decode_fail("decode_to_hw");
                return nullptr;
            }
            if (draining_) return nullptr;
            for (;;) {
                const auto rd_t0 = std::chrono::steady_clock::now();
                const int r = av_read_frame(fmt_ctx_, packet_);
                read_stall_tick(std::chrono::duration<double, std::milli>(
                                    std::chrono::steady_clock::now() - rd_t0).count());
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
    // Draining latched from an earlier read-error/EOF: from here on every call
    // returns null in microseconds and no seek resets it — log the very first
    // occurrence so a permanent GPU-fastpath death is visible in one line.
    if (draining_) {
        static bool logged_drain_lock = false;
        if (!logged_drain_lock) {
            logged_drain_lock = true;
            log::log_warning("[dec] decode_to_hw LATCHED DRAINING target=%lld last=%lld next=%lld "
                             "soft_only=%d engaged=%d hw=%d fmt=%d (returning null every call)",
                             (long long)target, (long long)last_frame_, (long long)next_frame_,
                             soft_only_ ? 1 : 0, hw_engaged_ ? 1 : 0, hw_avail_ ? 1 : 0, hw_pix_fmt_);
        }
        return nullptr;
    }
    return nullptr;
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
        CANVAS_LOG(
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
    const auto conv_t0 = std::chrono::steady_clock::now();
    // NOTE on colorspace/range: previously this swscale conversion passed NO
    // SWS_CS_* / range flag, so libswscale silently applied SWS_CS_DEFAULT == 5 ==
    // ITU601 (BT.601) to BT.709 sources — the two-matrix mismatch in issue #1.
    // Now pin the coefficients to BT.709 (matching the GPU shader and the tagged
    // streams) and honor the frame's real source range, producing full-range RGB
    // (the internal convention colorspace.hpp and the consumer shaders expect).
    // Pin the coefficients to the per-file color matrix and honor the file's
    // RESOLVED source range (tags reconciled with the luma probe), producing
    // full-range RGB — the internal convention colorspace.hpp and the consumer
    // shaders expect. dstRange is always 1 so the RGBA held in VideoFrame is
    // full-range regardless of the sample's quantization.
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
    const int src_range = (range_ == gpu::ColorRange::Full) ? 1 : 0;
    int sws_matrix = SWS_CS_ITU709;
    switch (matrix_) {
        case gpu::ColorMatrix::BT601: sws_matrix = SWS_CS_ITU601; break;
        case gpu::ColorMatrix::BT2020: sws_matrix = SWS_CS_BT2020; break;
        default: break;
    }
    const int* cs_coefs = sws_getCoefficients(sws_matrix);
    sws_setColorspaceDetails(sws_ctx_, cs_coefs, src_range, cs_coefs, 1,
                             0, 1 << 16, 1 << 16);
    // One line per (src format, src size) actually converted: enough to see the
    // resolved matrix/range pin on the CPU path without spamming per frame.
    static std::mutex sws_log_mu;
    static std::set<std::tuple<int, int, int>> sws_logged;  // fmt, w, h
    const bool first_cvt = [&] {
        std::lock_guard<std::mutex> lk(sws_log_mu);
        return sws_logged.insert({static_cast<int>(cvt->format), cvt->width, cvt->height}).second;
    }();
    if (first_cvt) {
        log::log_warning(
            "[decode] sws_rgba src_fmt=%d (%dx%d) flags=%s => sws_cs_id=%d srcRange=%d dstRange=1: "
            "resolved per-file spec matrix=%s range=%s (tags+probe)",
            static_cast<int>(cvt->format), cvt->width, cvt->height,
            scaled ? "scaled" : "unscaled",
            sws_matrix, src_range,
            gpu::color_matrix_name(matrix_), gpu::color_range_name(range_));
    }
    uint8_t* dst_data[] = {out->rgba.data()};
    int dst_linesize[] = {static_cast<int>(out->stride)};
    sws_scale(sws_ctx_, cvt->data, cvt->linesize, 0, cvt->height, dst_data, dst_linesize);
    convert_ms_ += std::chrono::duration<double, std::milli>(
                       std::chrono::steady_clock::now() - conv_t0).count();
    // Always-on ~1s CPU-conversion telemetry: sws (with/hw-download) cost per
    // RGBA frame, dims, and whether we downscaled (preview cap). Sustained
    // avg_ms here is pure CPU cost in the decode path — the thing software
    // playback is bound by when hw decode is off or the source is non-planar.
    static auto sws_agg_at = std::chrono::steady_clock::now();
    static int sws_agg_n = 0;
    static double sws_agg_ms = 0.0, sws_max_ms = 0.0;
    static int sws_scaled = 0;
    static int sws_sw = 0, sws_sh = 0, sws_dw = 0, sws_dh = 0;
    const double cvt_ms = std::chrono::duration<double, std::milli>(
                              std::chrono::steady_clock::now() - conv_t0).count();
    ++sws_agg_n;
    sws_agg_ms += cvt_ms;
    sws_max_ms = std::max(sws_max_ms, cvt_ms);
    if (scaled) ++sws_scaled;
    sws_sw = cvt->width; sws_sh = cvt->height;
    sws_dw = out_w; sws_dh = out_h;
    const auto sws_now = std::chrono::steady_clock::now();
    if (sws_agg_n == 1 || sws_now - sws_agg_at >= std::chrono::seconds(1)) {
        sws_agg_at = sws_now;
        log::log_warning(
            "[decode] sws_avg_ms=%.2f sws_max_ms=%.2f n=%d scaled=%d src=%dx%d dst=%dx%d",
            sws_agg_ms / static_cast<double>(sws_agg_n), sws_max_ms, sws_agg_n,
            sws_scaled, sws_sw, sws_sh, sws_dw, sws_dh);
        sws_agg_n = 0;
        sws_agg_ms = sws_max_ms = 0.0;
        sws_scaled = 0;
    }
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
CANVAS_LOG("vdecode container-seek target=%lld ms=%.2f ok=%d",
           static_cast<long long>(target), st_ms, frame != nullptr);
    if (!frame) return nullptr;
    next_frame_ = frame->frame_number + 1;
    return frame;
}

VideoFramePtr VideoDecoder::decode_to_frame(int64_t target, int max_output_dim) {
    if (!codec_ctx_ || frame_rate_ <= 0.0) return nullptr;
    set_output_dim(max_output_dim);
    refine_last_frame();

    // Frozen-tail hold: requesting past the stream's final frame used to
    // container-seek and re-decode the identical last frame on every call
    // (~90ms each) for the whole audio-only share of a project. Serve the
    // cached final frame instead; build the cache once by decoding it exactly.
    if (last_frame_ > 0 && target > last_frame_) {
        if (hold_rgba_ && hold_rgba_src_ == last_frame_ && hold_rgba_dim_ == out_max_dim_)
            return hold_rgba_;
        target = last_frame_;
    } else {
        target = clamp_target(target);
    }

    const auto dbg_start = std::chrono::steady_clock::now();
    const bool dbg_hw = hw_pix_fmt_ != AV_PIX_FMT_NONE;
    VideoFramePtr dbg_out = nullptr;

// Give sequential decode a small window of forward progress to avoid a
    // random seek on the common playback path. If we've already moved past the
    // target or it's far ahead, seek — a big sequential forward jump would block
    // for (distance * ~ms/frame) and stall scrubbing, while a keyframe seek
    // bounds decode-forward to a single GOP.
    if (target >= next_frame_ && target - next_frame_ < 64) {
        const auto seq_t0 = std::chrono::steady_clock::now();
        VideoFramePtr frame = decode_forward_to(target, kFullResMaxOver);
        const double seq_ms = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - seq_t0).count();
        if (frame) {
            next_frame_ = frame->frame_number + 1;
            dbg_out = std::move(frame);
            ++path_seq_;
            path_seq_ms_ += seq_ms;
        } else {
            // Fall through to a (re)seek if sequential decode stalled.
        }
    }
    if (!dbg_out) {
        const auto seek_t0 = std::chrono::steady_clock::now();
        dbg_out = seek_to_frame_indexed(target, max_output_dim);
        ++path_seeks_;
        path_seek_ms_ += std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - seek_t0).count();
    }

    if (dbg_out) {
        const double dbg_ms = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - dbg_start).count();
        CANVAS_LOG("decode: frame %lld took %.2f ms hw=%d", (long long)dbg_out->frame_number,
               dbg_ms, (int)dbg_hw);
        if (last_frame_ > 0 && dbg_out->frame_number >= last_frame_) {
            hold_rgba_ = dbg_out;
            hold_rgba_src_ = dbg_out->frame_number;
            hold_rgba_dim_ = out_max_dim_;
        }
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
        const auto ib_t0 = std::chrono::steady_clock::now();
        auto built = build_iframe_sync(path, stream_tb, fps, vstream);
        const double ib_ms = std::chrono::duration<double, std::milli>(
                                 std::chrono::steady_clock::now() - ib_t0).count();
        std::lock_guard<std::mutex> lk2(s_iframe_mtx);
        if (built && built->size() > 1 && !s_iframe_cache.count(path))
            s_iframe_cache[path] = std::move(built);
        s_iframe_inflight.erase(path);
        // Always-on: the I-frame index build is a one-shot, seconds-scale scan
        // that first-touch scrub latency and export seek cost are gated on. A
        // multi-second build on a long file is expected; repeated builds of the
        // SAME path mean the in-flight/cache dedupe broke.
        const double gop_s = (built && built->size() > 2)
            ? (built->back().frame - built->front().frame) /
                  (static_cast<double>(built->size()) * std::max(fps, 1.0))
            : 0.0;
        ::canvas::core::log::log_warning(
            "[dec] iframe_index path=%s entries=%zu ms=%.0f gop_secs=%.2f cached=%d",
            path.c_str(), built ? built->size() : 0, ib_ms, gop_s,
            s_iframe_cache.count(path) > 0);
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
    CANVAS_LOG(
                       "vdecode scrub indexed-seek target=%lld iframe=%lld gop_secs=%.3f max=%d ms=%.2f ok=%d",
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
            const auto rd_t0 = std::chrono::steady_clock::now();
            const int r = av_read_frame(audio_fmt_ctx_, audio_packet_);
            read_stall_tick(std::chrono::duration<double, std::milli>(
                                std::chrono::steady_clock::now() - rd_t0).count());
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

VideoDecoder::PathStats VideoDecoder::path_stats() const {
    return PathStats{path_seq_, path_seeks_, path_seq_ms_, path_seek_ms_, convert_ms_};
}

VideoDecoder::PathStats VideoDecoder::take_path_stats() {
    const PathStats out = path_stats();
    path_seq_ = path_seeks_ = 0;
    path_seq_ms_ = path_seek_ms_ = 0.0;
    convert_ms_ = 0.0;
    return out;
}

}
