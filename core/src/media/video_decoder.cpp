#include "canvas/core/media/video_decoder.hpp"
#include "canvas/core/media/vaapi/driver.hpp"
#include "canvas/core/media/vaapi/export.hpp"
#include "canvas/core/util/color_log.hpp"
#include "canvas/core/util/log.hpp"

#include <algorithm>
#include <array>
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

#ifdef CANVAS_HAVE_VAAPI
extern "C" {
#include <libavutil/hwcontext_vaapi.h>
#include <va/va.h>
}
#endif

namespace canvas::core {

namespace {

// Driver identity for a VAAPI display (an empty string on failure). Only
// meaningful when the zero-copy VAAPI backend is compiled in.
#ifdef CANVAS_HAVE_VAAPI
std::string va_driver_name(VADisplay dpy) {
    if (!dpy) return {};
    const char* raw = vaQueryVendorString(dpy);
    if (!raw || !raw[0]) return {};
    return raw;
}
#endif

// Whether `pix_fmt` is a device-memory format the GPU NV12 fast path can hand
// to the display directly (CUDA device frames, and VAAPI surfaces when the
// zero-copy backend is built).
bool hw_fast_path_pixfmt(const int pix_fmt) noexcept {
#ifdef CANVAS_HAVE_VAAPI
    return pix_fmt == AV_PIX_FMT_CUDA || pix_fmt == AV_PIX_FMT_VAAPI;
#else
    return pix_fmt == AV_PIX_FMT_CUDA;
#endif
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

VideoDecoder::VideoDecoder(VideoDecoder&& o) noexcept { *this = std::move(o); }

VideoDecoder& VideoDecoder::operator=(VideoDecoder&& o) noexcept {
    if (this == &o) return *this;
    close();
    // Steal the shared demux/decode session and the software decoder. The
    // SoftDecoder's move nulls its own demux_ pointer; we re-point it at the
    // (heaped) DemuxState we just took over, whose address is stable.
    demux_state_ = std::move(o.demux_state_);
    sw_decoder_ = std::move(o.sw_decoder_);
    if (demux_state_) sw_decoder_.set_demux(demux_state_.get());
    hold_hw_ = o.hold_hw_;
    o.hold_hw_ = nullptr;
    hold_hw_src_ = o.hold_hw_src_;
    o.hold_hw_src_ = -1;
    retain_hw_ = o.retain_hw_;
    o.retain_hw_ = nullptr;
    retain_hw_src_ = o.retain_hw_src_;
    o.retain_hw_src_ = -1;
    audio_fmt_ctx_ = o.audio_fmt_ctx_;
    o.audio_fmt_ctx_ = nullptr;
    audio_codec_ = o.audio_codec_;
    o.audio_codec_ = nullptr;
    swr_ctx_ = o.swr_ctx_;
    o.swr_ctx_ = nullptr;
    audio_frame_ = o.audio_frame_;
    o.audio_frame_ = nullptr;
    audio_packet_ = o.audio_packet_;
    o.audio_packet_ = nullptr;
    audio_sample_rate_ = o.audio_sample_rate_;
    o.audio_sample_rate_ = 0;
    audio_channels_ = o.audio_channels_;
    o.audio_channels_ = 0;
    audio_samples_total_ = o.audio_samples_total_;
    o.audio_samples_total_ = 0;
    audio_next_sample_ = o.audio_next_sample_;
    o.audio_next_sample_ = 0;
    audio_tb_ = o.audio_tb_;
    // Hardware engagement state. The path key (DemuxState::path) traveled with
    // the demux session, so the adopted decoder keeps its I-frame index cache
    // key + counters.
    hw_pix_fmt_ = o.hw_pix_fmt_;
    o.hw_pix_fmt_ = AV_PIX_FMT_NONE;
    hw_avail_ = o.hw_avail_;
    o.hw_avail_ = false;
    hw_engaged_ = o.hw_engaged_;
    o.hw_engaged_ = false;
    soft_only_ = o.soft_only_;
    o.soft_only_ = false;
    return *this;
}

bool VideoDecoder::open(const std::string& path, std::string* error,
                        const AVBufferRef* hw_device_ctx,
                        const char* gpu_label) {
    close();
    demux_state_ = std::make_unique<DemuxState>();
    DemuxState& d = *demux_state_;
    sw_decoder_.set_demux(demux_state_.get());
    d.path = path;
    hw_gpu_label_ = gpu_label ? gpu_label : "";
    const auto open_t0 = std::chrono::steady_clock::now();

    AVFormatContext* ctx = nullptr;
    if (avformat_open_input(&ctx, path.c_str(), nullptr, nullptr) < 0) {
        if (error) *error = "failed to open '" + path + "'";
        return false;
    }
    d.fmt_ctx = ctx;

    if (avformat_find_stream_info(d.fmt_ctx, nullptr) < 0) {
        if (error) *error = "failed to read stream info";
        close();
        return false;
    }

    d.video_stream = av_find_best_stream(d.fmt_ctx, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (d.video_stream < 0) {
        if (error) *error = "no video stream found";
        close();
        return false;
    }

    const AVStream* stream = d.fmt_ctx->streams[d.video_stream];
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
    const char* dev_type_name = dev_type != AV_HWDEVICE_TYPE_NONE
        ? av_hwdevice_get_type_name(dev_type)
        : nullptr;
    hw_type_name_ = dev_type_name ? dev_type_name : "";
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

    d.codec_ctx = avcodec_alloc_context3(codec);
    if (!d.codec_ctx || avcodec_parameters_to_context(d.codec_ctx, stream->codecpar) < 0) {
        if (error) *error = "failed to initialize decoder";
        close();
        return false;
    }

    // If a shared hardware device is available and this codec supports it, enable
    // hardware decode. The hw pixel format is remembered so the download path
    // (convert_to_rgba) knows how to pull data back to the CPU.
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
                d.codec_ctx->hw_device_ctx = av_buffer_ref(hw_device_ctx);
                hw_pix_fmt_ = config->pix_fmt;
                break;
            }
        }
        if (hw_pix_fmt_ != AV_PIX_FMT_NONE) {
            d.codec_ctx->get_format = [](AVCodecContext* c, const AVPixelFormat* pix_fmts) {
                for (const AVPixelFormat* p = pix_fmts; *p != AV_PIX_FMT_NONE; ++p) {
                    if (*p == static_cast<AVPixelFormat>(reinterpret_cast<intptr_t>(c->opaque)))
                        return *p;
                }
                return AV_PIX_FMT_NONE;
            };
            // Stash the hw pixel format for the get_format callback via opaque.
            d.codec_ctx->opaque = reinterpret_cast<void*>(static_cast<intptr_t>(hw_pix_fmt_));
        }
    }

    if (avcodec_open2(d.codec_ctx, codec, nullptr) < 0) {
        if (error) *error = "failed to initialize decoder";
        close();
        return false;
    }
    d.codec_ctx->thread_count = 0;

    // Hardware decode is on when we found an AVCodecHWConfig for the shared device
    // (hw_pix_fmt_ set). Hardware frames carry a hw_frames_ctx and get
    // downloaded in convert_to_rgba; if the codec can't actually decode in
    // hardware, FFmpeg transparently emits software frames instead.
    hw_avail_ = hw_pix_fmt_ != AV_PIX_FMT_NONE;
    d.hw_pix_fmt = hw_pix_fmt_;
    d.hw_avail = hw_avail_;

    d.av_frame = av_frame_alloc();
    d.packet = av_packet_alloc();
    if (!d.av_frame || !d.packet) {
        if (error) *error = "out of memory";
        close();
        return false;
    }

    d.stream_tb = stream->time_base;
    d.width = d.codec_ctx->width;
    d.height = d.codec_ctx->height;

    const AVRational fr = stream->avg_frame_rate.num != 0 ? stream->avg_frame_rate : stream->r_frame_rate;
    d.frame_rate = fr.num > 0 ? av_q2d(fr) : 0.0;
    d.duration_seconds = d.fmt_ctx->duration > 0
        ? static_cast<double>(d.fmt_ctx->duration) / AV_TIME_BASE
        : (stream->duration > 0 ? static_cast<double>(stream->duration) * av_q2d(d.stream_tb) : 0.0);
    d.total_frames = d.frame_rate > 0.0 && d.duration_seconds > 0.0
        ? static_cast<int64_t>(std::llround(d.duration_seconds * d.frame_rate))
        : -1;
    // Seed the decode clamp from the open-time duration estimate so behavior is
    // unchanged when the container hands a duration; refine_last_frame() and the
    // EOF sites tighten it later when only the stream extent is known.
    d.last_frame = d.total_frames > 0 ? d.total_frames - 1 : -1;
    CANVAS_LOG("decode open: '%s' %dx%d fps=%.3f dur=%.3fs hw=%s hw_pix=%d",
           path.c_str(), d.width, d.height, d.frame_rate, d.duration_seconds,
           hw_avail_ ? hw_type_name_.c_str() : "sw", hw_pix_fmt_);

    // --- Optional audio stream ---
    audio_stream_ = -1;
    audio_sample_rate_ = 0;
    audio_channels_ = 0;
    audio_samples_total_ = 0;
    // Open a *separate* demuxer for audio. Video lookahead advances the shared
    // fmt_ctx and discards non-video packets, so audio needs its own container
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
            d.matrix = gpu::ColorMatrix::BT601;
            break;
        case AVCOL_SPC_BT2020_NCL:
        case AVCOL_SPC_BT2020_CL:
            d.matrix = gpu::ColorMatrix::BT2020;
            break;
        default:
            d.matrix = gpu::ColorMatrix::BT709;
            break;
    }
    if (stream->codecpar->color_space == AVCOL_SPC_UNSPECIFIED) {
        switch (stream->codecpar->color_primaries) {
            case AVCOL_PRI_BT470BG:
            case AVCOL_PRI_SMPTE170M:
                d.matrix = gpu::ColorMatrix::BT601;
                break;
            case AVCOL_PRI_BT2020:
                d.matrix = gpu::ColorMatrix::BT2020;
                break;
            default:
                break;
        }
    }
    d.range = (stream->codecpar->color_range == AVCOL_RANGE_JPEG) ? gpu::ColorRange::Full
                                                                  : gpu::ColorRange::Limited;
    const char* range_origin = "tags";
    if (d.range == gpu::ColorRange::Limited) {
        const gpu::ColorRange probed = cached_color_range_probe(path);
        if (probed == gpu::ColorRange::Full) {
            d.range = gpu::ColorRange::Full;
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
    const bool gpu_tag = hw_avail_ && !hw_gpu_label_.empty();
    ::canvas::core::log::log_warning(
        "[dec] open media=%s ms=%.0f dims=%dx%d fps=%.3f frames=%lld hw=%s%s%s "
        "audio=%s tags=range:%s/matrix:%s/trc:%s",
        path.c_str(), open_ms, d.width, d.height, d.frame_rate,
        static_cast<long long>(d.total_frames), hw_avail_ ? hw_type_name_.c_str() : "sw",
        gpu_tag ? " gpu=\"" : "", gpu_tag ? hw_gpu_label_.c_str() : "",
        audio_stream_ >= 0 ? "yes" : "no",
        color_tag_str(stream->codecpar->color_range),
        matrix_tag_str(stream->codecpar->color_space),
        trc_tag_str(stream->codecpar->color_trc));
    log::log_warning(
        "[dec] color spec resolved: matrix=%s range=%s (%s)",
        gpu::color_matrix_name(d.matrix), gpu::color_range_name(d.range), range_origin);

    // Always-on color archive (color.log, no gate, not canvas_debug.log): the
    // per-media baseline gathered BEFORE any grading — what the source
    // declared versus what the probe resolved, so a later wheel/curve change
    // can be judged against the frame's actual color spec.
    CANVAS_COLOR_LOG(
        "[media] open path=%s codec=%s dims=%dx%d tags=range:%s/matrix:%s/trc:%s "
        "resolved=matrix:%s/range:%s verdict=%s",
        path.c_str(), avcodec_get_name(stream->codecpar->codec_id), d.width, d.height,
        color_tag_str(stream->codecpar->color_range),
        matrix_tag_str(stream->codecpar->color_space),
        trc_tag_str(stream->codecpar->color_trc), gpu::color_matrix_name(d.matrix),
        gpu::color_range_name(d.range), range_origin);

    d.next_frame = 0;
    d.draining = false;
    hw_engaged_ = false;
    soft_only_ = false;
    return true;
}

bool VideoDecoder::is_still_picture() const {
    if (!demux_state_ || demux_state_->video_stream < 0) return false;
    if (demux_state_->fmt_ctx->streams[demux_state_->video_stream]->disposition &
        AV_DISPOSITION_ATTACHED_PIC)
        return true;
    // No disposition hint (ogg/flac-style art): a still is demuxed as a single
    // video packet, while motion footage hands back one per frame. Count packets
    // for the picked stream with a demux-only scan — no decoding, and it stops
    // at the second video packet.
    AVPacket* pkt = av_packet_alloc();
    int64_t video_packets = 0;
    while (av_read_frame(demux_state_->fmt_ctx, pkt) >= 0) {
        if (pkt->stream_index == demux_state_->video_stream && ++video_packets > 1) break;
        av_packet_unref(pkt);
    }
    av_packet_free(&pkt);
    return video_packets <= 1;
}

void VideoDecoder::close() {
    CANVAS_LOG("video_decoder: close '%s' hw=%d next_frame=%lld",
           demux_state_ ? demux_state_->path.c_str() : "",
           (int)hw_avail_,
           demux_state_ ? (long long)demux_state_->next_frame : 0);
    if (hold_hw_) av_frame_free(&hold_hw_);
    hold_hw_src_ = -1;
    if (retain_hw_) av_frame_free(&retain_hw_);
    retain_hw_src_ = -1;
    if (audio_packet_) av_packet_free(&audio_packet_);
    if (audio_frame_) av_frame_free(&audio_frame_);
    if (audio_codec_) avcodec_free_context(&audio_codec_);
    if (swr_ctx_) swr_free(&swr_ctx_);
    if (audio_fmt_ctx_) avformat_close_input(&audio_fmt_ctx_);
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
    // Software decoder first (frees the sws context + hold, clears its demux_
    // back-pointer), then the shared demux session (frees the FFmpeg contexts).
    sw_decoder_.close();
    if (demux_state_) demux_state_->close();
    demux_state_.reset();
    hw_pix_fmt_ = AV_PIX_FMT_NONE;
    hw_avail_ = false;
    hw_engaged_ = false;
    soft_only_ = false;
}

std::string VideoDecoder::video_stream_summary() const {
    std::string s = "streams=" + std::to_string(nb_streams()) +
                    " video_stream=" + std::to_string(video_stream_index());
    if (demux_state_ && demux_state_->codec_ctx)
        s += " picked_codec=" +
             std::string(avcodec_get_name(demux_state_->codec_ctx->codec_id));
    if (demux_state_ && demux_state_->fmt_ctx) {
        int vid = 1;
        for (unsigned i = 0; i < demux_state_->fmt_ctx->nb_streams; ++i) {
            const AVStream* st = demux_state_->fmt_ctx->streams[i];
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

VideoFramePtr VideoDecoder::decode_next() { return sw_decoder_.decode_next(); }

void VideoDecoder::set_output_dim(int max_output_dim) {
    sw_decoder_.set_output_dim(max_output_dim);
}

VideoFramePtr VideoDecoder::decode_to_frame(int64_t target_frame, int max_output_dim) {
    return sw_decoder_.decode_to_frame(target_frame, max_output_dim);
}

VideoFramePtr VideoDecoder::seek_to_frame(int64_t target_frame, int max_output_dim) {
    return sw_decoder_.seek_to_frame(target_frame, max_output_dim);
}

void VideoDecoder::build_iframe_index() {
    if (demux_state_) demux_state_->build_iframe_index();
}

const IframeEntry* VideoDecoder::iframe_at_or_before(int64_t target) const {
    return demux_state_ ? demux_state_->iframe_at_or_before(target) : nullptr;
}

VideoFramePtr VideoDecoder::seek_to_frame_indexed(int64_t target, int max_output_dim) {
    return sw_decoder_.seek_to_frame_indexed(target, max_output_dim);
}

const AVFrame* VideoDecoder::decode_to_hw(const int64_t target, const int max_over) {
    if (!demux_state_ || !demux_state_->codec_ctx || demux_state_->frame_rate <= 0.0)
        return nullptr;
    DemuxState& d = *demux_state_;
    d.refine_last_frame();
    // This path only serves hardware-decoded frames (device memory: NV12 on
    // CUDA, or a VAAPI surface frame); software decode must use the CPU RGBA
    // path. hw_pix_fmt_ is the *hwaccel* format negotiated in open()
    // (AV_PIX_FMT_CUDA / AV_PIX_FMT_VAAPI) — the tag AVFrame::format carries
    // for device-memory frames. AV_PIX_FMT_NV12 is the nested sw_format inside
    // hw_frames_ctx, never the frame's own format, so comparing against it here
    // used to fail and this function returned null before decoding at all.
    if (!hw_avail_ || !hw_fast_path_pixfmt(hw_pix_fmt_)) return nullptr;

    // Frozen-tail hold: once the stream's final frame is known (d.last_frame),
    // a request past it must not re-walk to EOF and fail on every call — the
    // exporter then silently drops to per-frame CPU re-seeks (~90ms/frame) for
    // the whole audio-only tail. Serve a persistent ref of the last real frame
    // instead, built once by re-anchoring on the owning keyframe.
    if (d.last_frame > 0 && target > d.last_frame) {
        if (!hold_hw_ || hold_hw_src_ != d.last_frame) {
            const IframeEntry* entry = d.iframe_at_or_before(d.last_frame);
            if (entry) {
                d.container_seek_seconds(entry->pts_seconds);
            } else if (d.frame_rate > 0.0) {
                d.container_seek_seconds(d.last_frame / d.frame_rate);
            }
            const AVFrame* last = decode_to_hw(d.last_frame, 0);
            if (last && last->format == hw_pix_fmt_) {
                if (!hold_hw_) hold_hw_ = av_frame_alloc();
                if (hold_hw_ && av_frame_ref(hold_hw_, last) == 0) {
                    hold_hw_src_ = d.last_frame;
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
    const int64_t delta = target - d.next_frame;
    if (delta >= 64 || -delta >= 64) {
        const int64_t prev_next = d.next_frame;
        const IframeEntry* entry = d.iframe_at_or_before(target);
        if (entry) {
            d.container_seek_seconds(entry->pts_seconds);
        } else if (d.frame_rate > 0.0 && target > 0) {
            d.container_seek_seconds(target / d.frame_rate);
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
            const IframeEntry* entry = d.iframe_at_or_before(target);
            if (entry) {
                d.container_seek_seconds(entry->pts_seconds);
                ::canvas::core::log::log_warning(
                    "[dec] HW-ENGAGE target=%lld iframe=%lld gop_secs=%.3f",
                    (long long)target, (long long)entry->frame, entry->pts_seconds);
            } else if (d.frame_rate > 0.0 && target > 0) {
                d.container_seek_seconds(target / d.frame_rate);
            } else {
                break;
            }
            fast_over = 0;
        }
        for (;;) {
            const int ret = avcodec_receive_frame(d.codec_ctx, d.av_frame);
            if (ret == 0) {
                int64_t ticks = d.av_frame->best_effort_timestamp;
                if (ticks == AV_NOPTS_VALUE) ticks = d.av_frame->pts;
                const bool have_ts = ticks != AV_NOPTS_VALUE;
                const double secs = have_ts
                    ? static_cast<double>(ticks) * av_q2d(d.stream_tb)
                    : static_cast<double>(d.next_frame) / d.frame_rate;
                const int64_t number =
                    std::max<int64_t>(static_cast<int64_t>(std::llround(secs * d.frame_rate)), 0);

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
                        d.next_frame = number + 1;
                        return d.av_frame;
                    }
                    av_frame_unref(d.av_frame);
                    d.next_frame = number + 1;
                    continue;
                }
                // Target (or next frame at/after it) on the device.
                d.next_frame = number + 1;
                if (d.av_frame->format != hw_pix_fmt_) {
                    // SOFTWARE frame from a hardware-configured decoder (mid-GOP
                    // entry; the owning keyframe never primed the CUDA session). On
                    // the first attempt re-anchor so NVDEC engages; if the retry
                    // still yields software, latch soft_only_ and hand it back — the
                    // caller's GPU composite will fail and fall back to CPU RGBA,
                    // which is exactly the behavior this re-anchor fixes.
                    if (attempt == 0 && !hw_engaged_) {
                        av_frame_unref(d.av_frame);
                        CANVAS_LOG("video_decoder: hw emitting software target=%lld got=%lld; "
                                   "re-anchoring keyframe",
                                   (long long)target, (long long)number);
                        break;
                    }
                    if (attempt == 1) soft_only_ = true;
                    return d.av_frame;
                }
                hw_engaged_ = true;
                CANVAS_LOG("video_decoder: decode_to_hw OK target=%lld got=%lld",
                       (long long)target, (long long)number);
                decode_ok();
                if (!retain_hw_) retain_hw_ = av_frame_alloc();
                // av_frame_ref() requires a CLEAN destination (it neither unrefs
                // nor overwrites existing buffer refs); re-ref'ing the retained
                // copy without av_frame_unref() first leaked one NVDEC device
                // surface per decode here — ~5.5MB × 30fps ≈ 166MB/s of VRAM,
                // enough to fill the card in ~90s of playback.
                if (retain_hw_) av_frame_unref(retain_hw_);
                if (retain_hw_ && av_frame_ref(retain_hw_, d.av_frame) == 0) {
                    retain_hw_src_ = number;
                } else {
                    av_frame_free(&retain_hw_);
                    retain_hw_src_ = -1;
                }
                return d.av_frame;
            }
            if (ret == AVERROR_EOF) {
                if (d.next_frame > 0 && (d.last_frame < 0 || d.next_frame - 1 < d.last_frame)) {
                    const int64_t old_last = d.last_frame;
                    d.last_frame = d.next_frame - 1;
                    log::log_warning(
                        "[dec] HW-EOF srct=%-9lld hit=%-9lld last %-9lld -> %-9lld "
                        "engaged=%d soft_only=%d hw=%d fmt=%d (latched, no re-anchor)",
                        (long long)target, (long long)(d.next_frame - 1),
                        (long long)old_last, (long long)d.last_frame,
                        hw_engaged_ ? 1 : 0, soft_only_ ? 1 : 0, hw_avail_ ? 1 : 0, hw_pix_fmt_);
                }
                CANVAS_LOG("video_decoder: decode_to_hw EOF at frame %lld", (long long)d.next_frame);
                return nullptr;
            }
            if (ret != AVERROR(EAGAIN)) {
                log::log_error("video_decoder: decode_to_hw ERROR ret=%d target=%lld",
                                ret, (long long)target);
                decode_fail("decode_to_hw");
                return nullptr;
            }
            if (d.draining) return nullptr;
            if (!d.read_video_packet()) continue;
        }
    }
    // Draining latched from an earlier read-error/EOF: from here on every call
    // returns null in microseconds and no seek resets it — log the very first
    // occurrence so a permanent GPU-fastpath death is visible in one line.
    if (d.draining) {
        static bool logged_drain_lock = false;
        if (!logged_drain_lock) {
            logged_drain_lock = true;
            log::log_warning("[dec] decode_to_hw LATCHED DRAINING target=%lld last=%lld next=%lld "
                             "soft_only=%d engaged=%d hw=%d fmt=%d (returning null every call)",
                             (long long)target, (long long)d.last_frame, (long long)d.next_frame,
                             soft_only_ ? 1 : 0, hw_engaged_ ? 1 : 0, hw_avail_ ? 1 : 0, hw_pix_fmt_);
        }
        return nullptr;
    }
    return nullptr;
}

const AVFrame* VideoDecoder::decode_to_hw_indexed(const int64_t target, const int max_over) {
    if (!demux_state_ || !demux_state_->codec_ctx || demux_state_->frame_rate <= 0.0)
        return nullptr;
    if (!hw_avail_ || !hw_fast_path_pixfmt(hw_pix_fmt_)) return nullptr;
    DemuxState& d = *demux_state_;
    d.refine_last_frame();
    const int64_t t = d.clamp_target(target);

    // One-frame sequential lookback: when consecutive timeline frames re-target
    // the same source (the B side of a 2:1 transition round-trips onto one
    // frame), the repeat arrives here with src < next_frame and would trigger
    // a container seek below — which resets next_frame to 0 and re-walks a
    // whole GOP (~54ms on 2K60). If decode_to_hw just served this exact frame,
    // return that retained copy and leave next_frame where the walk parked it,
    // so the next DISTINCT target keeps riding the cheap sequential path.
    if (retain_hw_ && t == retain_hw_src_ && d.next_frame > t) {
        CANVAS_LOG("video_decoder: retain-hit target=%lld (no re-seek)", (long long)t);
        return retain_hw_;
    }

    // Jump to the keyframe that owns this target so the forward walk only
    // traverses one Group of Pictures. Uses the built keyframe index; without
    // one, a plain container seek to the target's presentation time lands on
    // the keyframe at-or-before, which also anchors backward scrubs.
    //
    // Sequential lookback: a near-forward target (at-or-ahead of the in-flight
    // walk, within the same window decode_to_hw treats as "sequential") keeps
    // riding that walk instead of re-anchoring — the unconditional pre-seek
    // used to reset next_frame=0 and re-walk the whole GOP for a +1 step,
    // pinning every small forward scrub move at ~full-GOP cost. Rules:
    //   delta < 0  -> behind the walk: must re-anchor (a forward walk cannot go
    //                backward), keep the seek.
    //   delta >= 64 -> same stride decode_to_hw considers far: re-anchor on the
    //                owning I-frame so the walk stays bounded.
    //   else -> serve from / continue the in-flight walk; decode_to_hw's own
    //           window checks cannot disagree (identical 64 threshold).
    const int64_t delta = t - d.next_frame;
    if (delta < 0 || delta >= 64) {
        const IframeEntry* entry = d.iframe_at_or_before(t);
        if (entry) {
            d.container_seek_seconds(entry->pts_seconds);
            CANVAS_LOG(
                "vdecode hw-indexed target=%lld iframe=%lld gop_secs=%.3f max=%d",
                (long long)t, (long long)entry->frame, entry->pts_seconds, max_over);
        } else if (d.frame_rate > 0.0) {
            d.container_seek_seconds(t / d.frame_rate);
        }
    }
    const AVFrame* hw = decode_to_hw(t, max_over == 0 ? kFullResMaxOver : max_over);  // sets next_frame internally
    return hw;
}

vaapi::VaapiSurfacePtr VideoDecoder::vaapi_export_surface(const AVFrame* hw,
                                                          const int64_t frame_number) const {
#ifdef CANVAS_HAVE_VAAPI
    // Only a VAAPI-hardware-configured decoder has something to export; CUDA
    // frames are consumed by the host_nv12_from_hw path instead.
    if (!hw_avail_ || hw_pix_fmt_ != AV_PIX_FMT_VAAPI) return nullptr;
    if (!demux_state_ || !hw || hw->format != AV_PIX_FMT_VAAPI || !hw->buf[0] ||
        !demux_state_->codec_ctx->hw_device_ctx) {
        ::canvas::core::log::log_warning(
            "[vaapi] export: unexpected hw frame (fmt=%d buf=%p hwdev=%p) frame=%lld",
            hw ? hw->format : -1, (void*)(hw ? hw->buf[0] : nullptr),
            (void*)(demux_state_ ? demux_state_->codec_ctx->hw_device_ctx : nullptr),
            static_cast<long long>(frame_number));
        return nullptr;
    }
    const auto* hwdev = reinterpret_cast<const AVHWDeviceContext*>(
        demux_state_->codec_ctx->hw_device_ctx->data);
    if (!hwdev || hwdev->type != AV_HWDEVICE_TYPE_VAAPI) return nullptr;
    const auto* vactx = reinterpret_cast<const AVVAAPIDeviceContext*>(hwdev->hwctx);
    if (!vactx || !vactx->display) return nullptr;

    const VASurfaceID surface = static_cast<VASurfaceID>(reinterpret_cast<uintptr_t>(hw->data[3]));
    VADRMPRIMESurfaceDescriptor desc{};
    const VAStatus st = vaExportSurfaceHandle(vactx->display, surface, VA_EXPORT_SURFACE_READ_ONLY,
                                              VA_EXPORT_SURFACE_COMPOSED_LAYERS, &desc);
    if (st != VA_STATUS_SUCCESS) {
        ::canvas::core::log::log_warning(
            "[vaapi] vaExportSurfaceHandle FAILED st=%d surface=%u frame=%lld (falling back to CPU)",
            static_cast<int>(st), static_cast<unsigned>(surface),
            static_cast<long long>(frame_number));
        return nullptr;
    }
    const vaapi::Vendor vendor = vaapi::identify_vendor(va_driver_name(vactx->display));
    return vaapi::translate_descriptor(desc, frame_number, color_spec(), vendor, hw->buf[0]);
#else
    (void)hw;
    (void)frame_number;
    return nullptr;
#endif
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
    return PathStats{sw_decoder_.path_seq(), sw_decoder_.path_seeks(),
                     sw_decoder_.path_seq_ms(), sw_decoder_.path_seek_ms(),
                     sw_decoder_.convert_ms()};
}

VideoDecoder::PathStats VideoDecoder::take_path_stats() {
    const PathStats out = path_stats();
    sw_decoder_.reset_path_counters();
    return out;
}

}