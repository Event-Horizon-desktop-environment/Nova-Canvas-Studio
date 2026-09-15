#include "canvas/core/export/exporter.hpp"

#include "canvas/core/export/renderer.hpp"
#include "canvas/core/export/vaapi_encode.hpp"
#include "canvas/core/export/qsv_encode.hpp"
#include "canvas/core/gpu/colorspace.hpp"
#include "canvas/core/gpu/cuda_convert.hpp"
#include "canvas/core/media/hw_device.hpp"
#include "canvas/core/util/log.hpp"

#include <cctype>
#include <cstdlib>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/hwcontext.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libavutil/pixfmt.h>
#include <libswscale/swscale.h>
}

#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <istream>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <tuple>
#include <vector>

namespace canvas::core {

namespace {

// Live-preview cadence for ExportControl::on_frame: ~30 fps wall clock so the
// viewer mirrors the render without stalling the encode loop per frame.
constexpr std::chrono::milliseconds kPreviewPushInterval{33};

std::string av_err(int err) {
    char buf[AV_ERROR_MAX_STRING_SIZE] = {0};
    av_strerror(err, buf, sizeof(buf));
    return buf;
}

void apply_codec_extra(AVCodecContext* ctx, const std::string& extra) {
    std::istringstream iss(extra);
    std::string line;
    while (std::getline(iss, line)) {
        const std::size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string k = line.substr(0, eq);
        std::string v = line.substr(eq + 1);
        k.erase(0, k.find_first_not_of(" \t"));
        k.erase(k.find_last_not_of(" \t") + 1);
        v.erase(0, v.find_first_not_of(" \t"));
        v.erase(v.find_last_not_of(" \t") + 1);
        if (!k.empty()) av_opt_set(ctx->priv_data, k.c_str(), v.c_str(), 0);
    }
}

AVPixelFormat pick_video_fmt(const AVCodec* codec, bool* uses_hw) {
    *uses_hw = false;
    const enum AVPixelFormat* fmts = nullptr;
    int n = 0;
    if (avcodec_get_supported_config(NULL, codec, AV_CODEC_CONFIG_PIX_FORMAT,
                                     0, (const void**)&fmts, &n) == 0 && fmts) {
        AVPixelFormat hw = AV_PIX_FMT_NONE;
        AVPixelFormat sw = AV_PIX_FMT_NONE;
        for (unsigned i = 0; i < n; ++i) {
            const AVPixelFormat f = fmts[i];
            if (f == AV_PIX_FMT_NONE) continue;
            if (f == AV_PIX_FMT_CUDA || f == AV_PIX_FMT_VAAPI || f == AV_PIX_FMT_QSV ||
                f == AV_PIX_FMT_DRM_PRIME || f == AV_PIX_FMT_D3D11) {
                // A hardware-frame format: feed device frames to device encoders
                // (NVENC/VAAPI/QSV). NVENC lists YUV420P/NV12 before CUDA, so scan
                // the whole list rather than short-circuiting on the first sw format.
                *uses_hw = true;
                if (hw == AV_PIX_FMT_NONE) hw = f;
            } else if (f == AV_PIX_FMT_NV12 || f == AV_PIX_FMT_YUV420P) {
                if (sw == AV_PIX_FMT_NONE) sw = f;
            }
        }
        if (*uses_hw) return hw;         // feed the device the hw frame format
        if (sw != AV_PIX_FMT_NONE) return sw;
    }
    return AV_PIX_FMT_YUV420P;
}

std::string hw_device_for_codec(const std::string& codec) {
    if (codec.find("vaapi") != std::string::npos) return "vaapi";
    if (codec.find("qsv") != std::string::npos) return "qsv";
    if (codec.find("amf") != std::string::npos) return "amf";
    if (codec.find("nvenc") != std::string::npos) return "cuda";
    if (codec.find("vulkan") != std::string::npos) return "vulkan";
    return "";
}

bool is_hw_codec(const std::string& codec) {
    return !hw_device_for_codec(codec).empty();
}

// True when `f` is a hardware-frame format (fed to a device-backed encoder).
bool is_hw_pix_fmt(AVPixelFormat f) {
    return f == AV_PIX_FMT_CUDA || f == AV_PIX_FMT_VAAPI || f == AV_PIX_FMT_QSV ||
           f == AV_PIX_FMT_DRM_PRIME || f == AV_PIX_FMT_D3D11;
}

// Per-export device-side grade-LUT cache. The bake pointer (RenderSession caches
// one baked LUT per active clip via TrackDecoder::lut_for) identifies the grid,
// so a graded clip uploads once per export, not once per frame.
struct GpuGradeLut {
    const grade_graph::GradeLut3D* baked = nullptr;
    void* dev = nullptr;

    bool ensure(const RenderSession::GpuFrameInfo& gfi) {
        if (!gfi.grade || !gfi.grade->valid()) return false;
        if (baked == gfi.grade.get()) return dev != nullptr;
        release();
        dev = canvas::core::gpu::grade_lut_upload(gfi.grade->data.data(), gfi.grade->size);
        baked = dev ? gfi.grade.get() : nullptr;
        if (dev) {
            CANVAS_LOG("render: grade LUT uploaded to device size=%d seq=%llu",
                   gfi.grade->size, (unsigned long long)gfi.grade->change_seq);
        }
        return dev != nullptr;
    }

    gpu::GradeKernelParams params(const RenderSession::GpuFrameInfo& gfi) const {
        gpu::GradeKernelParams p;
        p.lut = static_cast<const float*>(dev);
        p.lut_size = gfi.grade ? gfi.grade->size : 0;
        const gpu::MatrixCoeffs k = gpu::matrix_coeffs(static_cast<gpu::ColorMatrix>(gfi.matrix),
                                                       static_cast<gpu::ColorRange>(gfi.range));
        p.r_cr = k.r_cr;
        p.g_cb = k.g_cb;
        p.g_cr = k.g_cr;
        p.b_cb = k.b_cb;
        p.range = gfi.range;
        return p;
    }

    void release() {
        if (dev) {
            canvas::core::gpu::grade_lut_free(dev);
            dev = nullptr;
        }
        baked = nullptr;
    }
};

}  // namespace

std::vector<std::string> available_hw_devices() {
    std::vector<std::string> out;
    const char* names[] = {"cuda", "vaapi", "qsv", "amf", nullptr};
    for (int i = 0; names[i]; ++i) {
        const AVHWDeviceType t = av_hwdevice_find_type_by_name(names[i]);
        if (t == AV_HWDEVICE_TYPE_NONE) continue;
        ::AVBufferRef* ref = nullptr;
        if (av_hwdevice_ctx_create(&ref, t, nullptr, nullptr, 0) == 0 && ref) {
            av_buffer_unref(&ref);
            out.push_back(names[i]);
        } else if (ref) {
            av_buffer_unref(&ref);
        }
    }
    return out;
}

std::vector<CodecInfo> list_video_codecs(const std::string& hw_device) {
    std::vector<CodecInfo> out;
    void* iter = nullptr;
    while (const AVCodec* c = av_codec_iterate(&iter)) {
        if (!av_codec_is_encoder(c) || c->type != AVMEDIA_TYPE_VIDEO) continue;
        const std::string name = c->name ? c->name : "";
        if (name.empty() || name == "gif") continue;
        // libopenh264 requires a non-standard FFmpeg build and its quality lags
        // libx264; never offer it regardless of what the linked FFmpeg has.
        if (name == "libopenh264") continue;

        const bool hw = is_hw_codec(name);
        if (!hw_device.empty()) {
            if (!hw || hw_device_for_codec(name) != hw_device) continue;
        } else {
            if (hw) continue; // software list only
        }

        CodecInfo ci;
        ci.name = name;
        ci.long_name = c->long_name ? c->long_name : "";
        ci.media_type = AVMEDIA_TYPE_VIDEO;
        ci.hw = hw;
        ci.hw_device = hw ? hw_device_for_codec(name) : "";
        out.push_back(std::move(ci));
    }
    return out;
}

std::vector<CodecInfo> list_audio_codecs() {
    std::vector<CodecInfo> out;
    void* iter = nullptr;
    while (const AVCodec* c = av_codec_iterate(&iter)) {
        if (!av_codec_is_encoder(c) || c->type != AVMEDIA_TYPE_AUDIO) continue;
        const std::string name = c->name ? c->name : "";
        if (name.empty()) continue;
        CodecInfo ci;
        ci.name = name;
        ci.long_name = c->long_name ? c->long_name : "";
        ci.media_type = AVMEDIA_TYPE_AUDIO;
        out.push_back(std::move(ci));
    }
    return out;
}

std::vector<ContainerInfo> list_containers() {
    std::vector<ContainerInfo> out;
    void* iter = nullptr;
    while (const AVOutputFormat* f = av_muxer_iterate(&iter)) {
        if (!f || !f->extensions || !*f->extensions) continue;
        ContainerInfo ci;
        ci.name = f->name ? f->name : "";
        ci.long_name = f->long_name ? f->long_name : "";
        ci.extensions = f->extensions;
        out.push_back(std::move(ci));
    }
    return out;
}

// Maps the Deliver UI's x264-style preset names to what each encoder family
// accepts: NVENC/VAAPI/QSV/AMF reject x264 names and use the p1..p7 NVENC
// presets; SVT-AV1 takes a numeric 0..13 preset (higher = faster, so the x264
// ordering is reversed); libx264/x265 take the name verbatim.
std::string nv_preset_for(const std::string& codec, const std::string& preset) {
    const std::string p = [&] {
        std::string q = preset;
        for (auto& c : q) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return q;
    }();

    // SVT-AV1: numeric 0..13 preset, higher = faster; reverse the x264 order.
    if (codec.find("svt") != std::string::npos || codec.find("av1") != std::string::npos) {
        // libaom takes 0..9 with the same higher = faster convention.
        if (p == "ultrafast" || p == "superfast") return "13";
        if (p == "veryfast") return "11";
        if (p == "fast") return "9";
        if (p == "faster") return "7";
        if (p == "medium") return "6";
        if (p == "slow") return "4";
        if (p == "veryslow") return "2";
        if (p == "placebo") return "0";
        return preset;
    }

    const bool hw = codec.find("nvenc") != std::string::npos ||
                    codec.find("vaapi") != std::string::npos ||
                    codec.find("qsv") != std::string::npos ||
                    codec.find("amf") != std::string::npos;
    if (!hw)
        return preset;  // software encoder: keep the x264 preset name verbatim

    // "Faster" stays on NVENC p2, not p4 — verified against a 44520-frame
    // 1440p60 HEVC 80 Mbps CBR render (~700 fps, matching this exporter at p2).
    if (p == "ultrafast" || p == "superfast") return "p1";
    if (p == "veryfast") return "p2";
    if (p == "faster") return "p2";
    if (p == "fast") return "p3";
    if (p == "medium") return "p5";
    if (p == "slow") return "p6";
    if (p == "veryslow" || p == "placebo") return "p7";
    // Already an NVENC preset or unknown: hand it through (av_opt_set no-ops on
    // invalid values rather than aborting).
    return preset;
}

// QSV presets are the x264-style INT 0..7 speed consts (veryslow=1 .. veryfast=7),
// NOT NVENC's p1..p7 strings — that is what `preset` on h264_qsv/hevc_qsv/av1_qsv
// accepts (see docs/vaapi.md §8). Unknown spellings fall through to the numeric
// passthrough: an already-numeric preset is handed to the encoder as-is
// (av_opt_set_int rejects out-of-range values rather than aborting).
int qsv_preset_for(const std::string& codec, const std::string& preset) {
    (void)codec;  // the QSV speed constants are shared across h264/hevc/av1
    const std::string p = [&] {
        std::string q = preset;
        for (auto& c : q) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return q;
    }();

    if (p == "ultrafast" || p == "superfast" || p == "veryfast") return 7;
    if (p == "faster") return 6;
    if (p == "fast") return 5;
    if (p == "medium") return 4;
    if (p == "slow") return 3;
    if (p == "slower") return 2;
    if (p == "veryslow" || p == "placebo") return 1;
    try {
        return std::stoi(preset);
    } catch (...) {
        return 4;  // unnamed fallback: medium
    }
}

bool export_project(const Project& project, const ExportSettings& s, ExportControl* control,
                    std::string* error) {
    const auto fail = [&](const std::string& m) {
        // Record failures unconditionally (not behind CANVAS_DEBUG) — render
        // errors are the top debugging target.
        ::canvas::core::log::log_error("render failure: %s", m.c_str());
        if (error) *error = m;
        return false;
    };
    CANVAS_LOG("render: begin out='%s' fmt=%s codec=%s w=%dx%d fps=%.3f frames=%lld audio=%s",
           s.output_path.c_str(), s.format.c_str(), s.video_codec.c_str(), s.width, s.height,
           s.fps, (long long)s.duration_frames,
           (s.audio_codec.empty() ? "no" : s.audio_codec.c_str()));
    if (s.output_path.empty())
        return fail("Output path is missing. Choose an output folder on the Deliver panel.");
    if (s.video_codec.empty())
        return fail("No video codec selected.");

    const AVOutputFormat* out_fmt = av_guess_format(s.format.c_str(), s.output_path.c_str(), nullptr);
    if (!out_fmt) out_fmt = av_guess_format(nullptr, s.output_path.c_str(), nullptr);
    if (!out_fmt) return fail("Unknown output container: " + s.format);

    AVFormatContext* oc = nullptr;
    if (avformat_alloc_output_context2(&oc, out_fmt, nullptr, s.output_path.c_str()) < 0 || !oc)
        return fail("Failed to allocate output context.");

    const auto cancelled = [&]() {
        return control && control->should_cancel && control->should_cancel();
    };
    const auto progress = [&](double p, const char* phase) {
        if (control && control->on_progress) control->on_progress(p, phase);
    };

    // Live-preview push: hands the fully-composited export frame to
    // ExportControl::on_frame at a fixed wall-clock cadence (~30 fps) so the
    // Deliver-page viewer can mirror the render without stalling the encode
    // loop or allocating a host frame at full encode speed. `last` starts at
    // epoch so the first frame previews immediately.
    struct PreviewThrottle {
        std::chrono::steady_clock::time_point last{};
        bool due() {
            const auto now = std::chrono::steady_clock::now();
            if (now - last < kPreviewPushInterval) return false;
            last = now;
            return true;
        }
    } preview_throttle;
    const auto push_preview = [&](const VideoFramePtr& vf) {
        if (!control || !control->on_frame || !vf) return;
        control->on_frame(vf);
    };

    const AVCodec* vcodec = avcodec_find_encoder_by_name(s.video_codec.c_str());
    if (!vcodec) { avformat_free_context(oc); return fail("Unknown video encoder: " + s.video_codec); }

    AVCodecContext* vctx = avcodec_alloc_context3(vcodec);
    if (!vctx) { avformat_free_context(oc); return fail("No video codec context."); }

    const std::string hw_device = hw_device_for_codec(s.video_codec);
    const bool hw_codec = !hw_device.empty();
    bool v_use_hw = false;
    const AVPixelFormat hw_pix = pick_video_fmt(vcodec, &v_use_hw);
    v_use_hw = v_use_hw && hw_codec;

    // Software pixel format the encoder consumes: NV12 for hardware encoders
    // (uploaded), else the codec's preferred sw format (e.g. ProRes
    // yuv422p10le) — forcing YUV420P blindly makes other-format-only encoders
    // fail to open.
    AVPixelFormat sw_pix = AV_PIX_FMT_YUV420P;
    if (hw_codec) {
        sw_pix = AV_PIX_FMT_NV12;
    } else if (hw_pix != AV_PIX_FMT_YUV420P && hw_pix != AV_PIX_FMT_NV12 &&
               !is_hw_pix_fmt(hw_pix)) {
        sw_pix = hw_pix;
    }

    vctx->width = s.width;
    vctx->height = s.height;
    const int ri = std::lround(s.fps);
    vctx->time_base = AVRational{1, std::max(1, ri)};
    vctx->framerate = AVRational{ri, 1};
    vctx->pix_fmt = v_use_hw ? hw_pix : sw_pix;
    vctx->gop_size = 120;
    vctx->max_b_frames = 0;
    // Compositing stays in the source's 8-bit limited-range bt709 space, so
    // stamp bt709 + TV range here. Missing tags make players guess the transfer
    // and slide into slow software colorspace conversion (stutter).
    vctx->color_range = AVCOL_RANGE_MPEG;          // limited / TV range
    vctx->colorspace = AVCOL_SPC_BT709;            // BT.709 primaries
    vctx->color_trc = AVCOL_TRC_BT709;             // BT.709 transfer
    vctx->color_primaries = AVCOL_PRI_BT709;       // BT.709 primaries
    // HEVC level is NOT set via vctx->level: NVENC ignores it and uses its own
    // private `level` option (defaults to "auto", under-picking to Main@3.1 for
    // 1440p60); it is tuned further down.
    // Emit SPS/PPS at open so extradata is present at write_header; without it
    // matroska/mxf reject the streams.
    vctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

    // crf drives quality modes (>= 0; "Best" is 0). Keep crf and bit_rate
    // mutually exclusive: constant-quality encoders reject both set together.
    // VAAPI encoders have no `crf` option — that push silently no-ops and the
    // export runs at the driver's default rate control (docs/vaapi.md §8.1).
    // Map the crf/rc intent onto VAAPI's rc_mode/global_quality knobs instead.
    if (s.crf >= 0) {
        if (is_vaapi_codec(s.video_codec)) {
            apply_vaapi_rate_control(vctx, vaapi_rate_control_from(
                s.video_codec, s.crf, s.vid_rc_mode, s.video_bitrate_kbps));
        } else if (is_qsv_codec(s.video_codec)) {
            // QSV has no `crf` priv (silent no-op) — its CRF analog is ICQ,
            // driven by avctx->global_quality (see docs/qsv.md §8.2).
            apply_qsv_rate_control(vctx, qsv_rate_control_from(
                s.video_codec, s.crf, s.vid_rc_mode, s.video_bitrate_kbps));
        } else {
            av_opt_set_int(vctx->priv_data, "crf", s.crf, 0);
        }
    }
    // Bitrate-driven mode: only when a rate is given AND crf is inactive.
    // Quality modes carry video_bitrate_kbps==0, so this is a no-op for them.
    const bool is_nvenc = s.video_codec.find("nvenc") != std::string::npos;
    if (s.video_bitrate_kbps > 0 && s.crf < 0) {
        const int64_t bps = static_cast<int64_t>(s.video_bitrate_kbps) * 1000;
        vctx->bit_rate = bps;
        // Tight VBV window holds the target average for CBR. NVENC needs `rc`
        // explicit or it silently falls back to a low default bitrate.
        const bool cbr = s.vid_rc_mode == "cbr";
        const bool vbr = s.vid_rc_mode == "vbr_target";
        if (cbr || vbr) {
            const int64_t max_bps =
                s.video_max_bitrate_kbps > 0
                    ? static_cast<int64_t>(s.video_max_bitrate_kbps) * 1000
                    : bps;
            // VBV: tight (== target) for CBR, looser (2× max) for bounded VBR peaks.
            const int64_t bufsize_bps = cbr ? max_bps : max_bps * 2;
            av_opt_set_int(vctx, "maxrate", max_bps, AV_OPT_SEARCH_CHILDREN);
            av_opt_set_int(vctx, "bufsize", bufsize_bps, AV_OPT_SEARCH_CHILDREN);
            if (is_nvenc)
                av_opt_set(vctx->priv_data, "rc", cbr ? "cbr" : "vbr", 0);
            else if (is_vaapi_codec(s.video_codec))
                // VAAPI needs rc_mode too, else VBR/CBR requests are ignored.
                av_opt_set(vctx->priv_data, "rc_mode", cbr ? "CBR" : "VBR", 0);
            else if (is_qsv_codec(s.video_codec))
                // QSV picks CBR/VBR from the global_quality vs. bitrate seam —
                // no rc_mode option, so route CBR intent through the ICQ/CBR
                // quality law instead (docs/qsv.md §8.2).
                av_opt_set_int(vctx->priv_data, "rc_mode", cbr ? 42 : 0, 0);
        }
    }
    // NVENC constant-QP: set rc + cq so quality is honored instead of the
    // default 8-12 Mbps ABR (terrible on 80 Mbps-scale work).
    if (is_nvenc && s.crf >= 0 && s.vid_rc_mode == "constqp") {
        av_opt_set(vctx->priv_data, "rc", "constqp", 0);
        av_opt_set_int(vctx->priv_data, "cq", s.crf, 0);
    }
    if (!s.preset.empty())
    {
        // QSV presets are the x264-style INT 0..7 consts (veryfast=7..veryslow=1=1),
        // NOT NVENC's p1..p7 strings. Push the INT law / name const; if no
        // spelling matches, fall through to nv_preset_for's numeric-passthrough.
        const std::string p = s.preset;
        if (is_qsv_codec(s.video_codec)) {
            if (!p.empty())
                av_opt_set_int(vctx->priv_data, "preset",
                               qsv_preset_for(s.video_codec, p), 0);
        } else if (is_vaapi_codec(s.video_codec)) {
            // VAAPI has NO `preset` private option (the p1..p7 nv_preset_for
            // spelling av_opt_set'd here previously was a silent no-op). The
            // real speed knobs are `async_depth` and h264-only `quality`, with
            // a measured law that differs from FFmpeg's help text — route the
            // x264-style name through the VAAPI mapping.
            const VaapiSpeed spd = vaapi_speed_for(s.video_codec, p);
            if (spd.async_depth > 0)
                av_opt_set_int(vctx->priv_data, "async_depth", spd.async_depth, 0);
            if (spd.quality > 0)
                av_opt_set_int(vctx->priv_data, "quality", spd.quality, 0);
        } else {
            av_opt_set(vctx->priv_data, "preset", nv_preset_for(s.video_codec, p).c_str(), 0);
        }
    }
    apply_codec_extra(vctx, s.extra);

    // Split-frame encoding: multi-engine GPUs (RTX 5070 Ti/5080/5090) encode
    // horizontal strips of each frame in parallel on one session. `1` = forced
    // (driver picks the strip count), `2` = hardcoded two-way; forced keeps one
    // setting portable across single/dual/triple-engine parts. HEVC/AV1 only;
    // the option silently no-ops where unsupported.
    const std::string vc = s.video_codec;
    if (vc.find("nvenc") != std::string::npos &&
        (vc.find("av1") != std::string::npos || vc.find("hevc") != std::string::npos ||
         vc.find("h265") != std::string::npos)) {
        av_opt_set_int(vctx->priv_data, "split_encode_mode", 1, 0);
    }
    // Stamp the highest HEVC level the output size/fps requires. NVENC's
    // "auto" under-picks Main@3.1 for 1440p60 (spec-invalid: 3.1 caps at 1080p),
    // which pushes strict decoders into software decode -> judder.
    if (is_nvenc && (vc.find("hevc") != std::string::npos ||
                     vc.find("h265") != std::string::npos)) {
        // Level from the stream's luma sample rate; values are NVENC's
        // H.264-style integers (150=3.1 ... 183=5.0).
        const double luma_sps =
            static_cast<double>(s.width) * static_cast<double>(s.height) * s.fps;
        int level = 183;  // Main@5.0: up to 2.56 Gsamples/s (covers 1440p60 @ 0.22G)
        if (luma_sps > 2.56e9) level = 186;   // 5.1
        if (luma_sps > 3.07e9) level = 200;   // 6.0
        av_opt_set_int(vctx->priv_data, "level", level, 0);
    }
    // NVENC needs roughly rc_lookahead + max_b_frames + 8 surfaces; hardcoding
    // too few makes FFmpeg bump it at open ("Defined rc_lookahead requires more
    // surfaces"). Derive it from whatever rc-lookahead is in the extra options.
    if (v_use_hw && !is_qsv_codec(s.video_codec)) {
        int rc_lookahead = 0;
        const std::string& ex = s.extra;
        const std::string key = "rc-lookahead=";
        std::size_t pos = ex.find(key);
        if (pos != std::string::npos) {
            std::size_t end = ex.find_first_of("\r\n", pos);
            const std::string val = ex.substr(pos + key.size(), end == std::string::npos
                                                                ? std::string::npos
                                                                : end - (pos + key.size()));
            rc_lookahead = std::atoi(val.c_str());
        }
        // rc_lookahead + B-frame depth (+8 base). max_b_frames=0 here, so only
        // the base pool plus lookahead is needed.
        const int surfaces = rc_lookahead > 0 ? rc_lookahead + 10 : 12;
        av_opt_set_int(vctx->priv_data, "surfaces", surfaces, 0);
    }

    ::AVBufferRef* hw_frames = nullptr;
    ::AVBufferRef* dec_dev = nullptr;  // device used for accelerated decode of sources
    if (v_use_hw) {
        const AVHWDeviceType dt = av_hwdevice_find_type_by_name(hw_device.c_str());
        ::AVBufferRef* dev = nullptr;
        if (dt != AV_HWDEVICE_TYPE_NONE) {
            // Same GPU pin the playback probes honor: a Settings-selected GPU
            // passes its render node / CUDA ordinal to the encoder device so
            // export lands on the same accelerator the user picked.
            const std::string& gpu_arg =
                (hw_device == canvas::core::HwDeviceManager::preferred_gpu_backend())
                    ? canvas::core::HwDeviceManager::preferred_device_arg()
                    : std::string{};
            av_hwdevice_ctx_create(&dev, dt,
                                   gpu_arg.empty() ? nullptr : gpu_arg.c_str(),
                                   nullptr, 0);
        }
        dec_dev = dev;
        if (dev) {
            ::AVBufferRef* fr = av_hwframe_ctx_alloc(dev);
            if (fr) {
                AVHWFramesContext* fc = reinterpret_cast<AVHWFramesContext*>(fr->data);
                if (fc) {
                    fc->format = hw_pix;
                    fc->sw_format = AV_PIX_FMT_NV12;
                    fc->width = s.width;
                    fc->height = s.height;
                    fc->initial_pool_size = 12;
                }
                if (av_hwframe_ctx_init(fr) == 0) {
                    hw_frames = fr;
                    vctx->hw_frames_ctx = av_buffer_ref(hw_frames);
                } else {
                    av_buffer_unref(&fr);
                }
            }
            // Keep `dec_dev` referenced for the decoder's lifetime: the frames
            // context holds its own device ref, so unref'ing here would drop the
            // decoder's device out from under it. Released at teardown.
        }
        if (!hw_frames) v_use_hw = false;
    }

    // Which hw-frame format the ENCODER actually consumes. `cuda_available()` is
    // a machine-global property (any NVIDIA GPU present), NOT a property of this
    // export's encoder frames — on a dual-vendor box where the only encoder is
    // VAAPI (AMD iGPU) but an NVIDIA GPU is also present, it used to fire the
    // CUDA composite kernels against AV_PIX_FMT_VAAPI encoder surfaces: the
    // encoder frame's data[] is a VASurfaceID, `frame_gpu` boxed that as
    // srcY/srcUV, and the kernels faulted asynchronously. The async fault read as
    // "landed" in telemetry while the surface was never written (garbage encoded
    // at ~4 fps), and the CPU fallback's cudaDeviceSynchronize surfaced the fault
    // as a per-frame alloc_miss. Both vendor paths therefore pick the encode-side
    // feed by the hw-frames FORMAT, never by machine-wide CUDA presence.
    const AVPixelFormat enc_hw_fmt = hw_frames
        ? reinterpret_cast<AVHWFramesContext*>(hw_frames->data)->format
        : AV_PIX_FMT_NONE;
    const bool enc_is_cuda = v_use_hw && hw_frames && enc_hw_fmt == AV_PIX_FMT_CUDA &&
                             canvas::core::gpu::cuda_available();
    const bool enc_is_vaapi = v_use_hw && hw_frames && enc_hw_fmt == AV_PIX_FMT_VAAPI;

    if (avcodec_open2(vctx, vcodec, nullptr) < 0) {
        if (hw_frames) av_buffer_unref(&hw_frames);
        avcodec_free_context(&vctx);
        avformat_free_context(oc);
        return fail("Failed to open video encoder: " + s.video_codec);
    }
    fprintf(stderr, "[dbg] after open: vctx time_base=%d/%d framerate=%d/%d pix_fmt=%d\n",
            vctx->time_base.num, vctx->time_base.den, vctx->framerate.num, vctx->framerate.den, vctx->pix_fmt);

    AVStream* vst = avformat_new_stream(oc, nullptr);
    if (!vst) { avcodec_free_context(&vctx); avformat_free_context(oc); return fail("No video stream."); }
    vst->id = static_cast<int>(oc->nb_streams);
    avcodec_parameters_from_context(vst->codecpar, vctx);
    vst->time_base = vctx->time_base;
    // Some muxers (mpeg, matroska) require stream frame-rate metadata or skew
    // the written header.
    vst->r_frame_rate = vctx->framerate;
    vst->avg_frame_rate = vctx->framerate;
    fprintf(stderr, "[dbg] vst->time_base set to %d/%d\n", vst->time_base.num, vst->time_base.den);

    // ---- audio ----
    const bool do_audio = !s.remove_audio && !s.audio_codec.empty() && s.duration_frames > 0;
    AVCodecContext* actx = nullptr;
    AVStream* ast = nullptr;
    int a_frame_size = 0;
    int a_src_samples_ = 0;
    if (do_audio) {
        const AVCodec* acodec = avcodec_find_encoder_by_name(s.audio_codec.c_str());
        if (!acodec) {
            avcodec_free_context(&vctx); avformat_free_context(oc);
            return fail("Unknown audio encoder: " + s.audio_codec);
        }
        actx = avcodec_alloc_context3(acodec);
        if (!actx) { avcodec_free_context(&vctx); avformat_free_context(oc); return fail("No audio ctx"); }
        actx->sample_rate = s.audio_sample_rate;
        av_channel_layout_default(&actx->ch_layout, s.audio_channels);
        actx->sample_fmt = AV_SAMPLE_FMT_FLTP;
        actx->bit_rate = static_cast<int64_t>(s.audio_bitrate_kbps) * 1000;
        if (avcodec_open2(actx, acodec, nullptr) < 0) {
            avcodec_free_context(&actx); avcodec_free_context(&vctx); avformat_free_context(oc);
            return fail("Failed to open audio encoder: " + s.audio_codec);
        }
        a_frame_size = std::max(1, actx->frame_size > 0 ? actx->frame_size : 1024);
        // Per-timeline-frame audio can exceed the encoder frame size (96kHz/60fps
        // = 1600 > AAC's 1024); size a_src for the larger or reads overrun planes.
        a_src_samples_ = std::max(a_frame_size,
            (int)std::max<int64_t>(1, (int64_t)std::llround((double)s.audio_sample_rate / s.fps)));
        ast = avformat_new_stream(oc, nullptr);
        if (!ast) { avcodec_free_context(&actx); avcodec_free_context(&vctx); avformat_free_context(oc); return fail("No audio stream."); }
        ast->id = static_cast<int>(oc->nb_streams);
        avcodec_parameters_from_context(ast->codecpar, actx);
        ast->time_base = AVRational{1, s.audio_sample_rate};
    }

    if (!(oc->oformat->flags & AVFMT_NOFILE)) {
        if (avio_open(&oc->pb, ("file:" + s.output_path).c_str(), AVIO_FLAG_WRITE) < 0) {
            avcodec_free_context(&actx); avcodec_free_context(&vctx); avformat_free_context(oc);
            return fail("Cannot open output file: " + s.output_path);
        }
    }

    const int wh_ret = avformat_write_header(oc, nullptr);
    if (wh_ret < 0) {
        char ebuf[128] = {0};
        av_strerror(wh_ret, ebuf, sizeof(ebuf));
        fprintf(stderr, "[wrh] ret=%d (%s) nstreams=%d\n", wh_ret, ebuf, (int)oc->nb_streams);
        for (unsigned i = 0; i < oc->nb_streams; ++i) {
            AVStream* st = oc->streams[i];
            AVCodecParameters* cp = st->codecpar;
            fprintf(stderr, "  st%u type=%d codec=%s fmt=%d tb=%d/%d w=%d h=%d sr=%d ch=%d\n",
                    i, cp->codec_type, cp->codec_id == AV_CODEC_ID_NONE ? "?" : "?",
                    cp->format, st->time_base.num, st->time_base.den,
                    cp->width, cp->height, cp->sample_rate,
                    cp->ch_layout.nb_channels);
        }
        avcodec_free_context(&actx); avcodec_free_context(&vctx);
        if (oc->pb) avio_closep(&oc->pb);
        avformat_free_context(oc);
        return fail("Failed to write container header.");
    }

    progress(0.0, "Encode");

    // Software format fed to the encoder: NV12 (+ upload) for hw encoders,
    // else vctx->pix_fmt exactly.
    const AVPixelFormat enc_sw_fmt = v_use_hw ? AV_PIX_FMT_NV12 : vctx->pix_fmt;
    SwsContext* sws = sws_getContext(s.width, s.height, AV_PIX_FMT_RGBA,
                                     s.width, s.height, enc_sw_fmt,
                                     SWS_BILINEAR, nullptr, nullptr, nullptr);
    if (!sws) {
        avcodec_free_context(&actx); avcodec_free_context(&vctx);
        if (oc->pb) avio_closep(&oc->pb);
        avformat_free_context(oc);
        return fail("Failed to create RGB->YUV conversion context.");
    }
    // Pin the conversion matrix so the encoded pixels match the stamped tags
    // (issue #1): previously this passed NO SWS_CS_* flag, so libswscale applied
    // its SWS_CS_DEFAULT (== ITU601 / BT.601) matrix to every RGB->YUV export no
    // matter what the output stream was tagged. Output is conceived full-range
    // RGB in, limited-range BT.709 YUV out (matches the tagged range:mpeg).
    const bool tagged_bt709 = (vctx->colorspace == AVCOL_SPC_BT709);
    const int conv_matrix = tagged_bt709 ? SWS_CS_ITU709 : SWS_CS_ITU601;
    const int* conv_coefs = sws_getCoefficients(conv_matrix);
    sws_setColorspaceDetails(sws, conv_coefs, 1, conv_coefs, 0,
                             0, 1 << 16, 1 << 16);
    // Tag-vs-conversion audit: output is stamped BT.709 limited (see configure
    // above); this swscale now uses the SAME matrix (sws_setColorspaceDetails
    // above), so a BT.709-tagged file is encoded with BT.709 coefficients.
    ::canvas::core::log::log_warning(
        "[export] color audit: out_tags=range:%s/matrix:%s/trc:%s ; rgba->%s "
        "sws_setColorspaceDetails matrix=%s srcRange=JPEG dstRange=MPEG — %s",
        vctx->color_range == AVCOL_RANGE_MPEG ? "mpeg" : "jpeg",
        vctx->colorspace == AVCOL_SPC_BT709 ? "bt709"
            : vctx->colorspace == AVCOL_SPC_BT470BG ? "bt601" : "other",
        vctx->color_trc == AVCOL_TRC_BT709 ? "bt709" : "other",
        av_get_pix_fmt_name(enc_sw_fmt),
        conv_matrix == SWS_CS_ITU709 ? "BT.709" : "BT.601",
        conv_matrix == SWS_CS_ITU709 ? "conversion matches stamped matrix"
                                    : "conversion matrix follows tagged colorspace");

    AVFrame* rgb = av_frame_alloc();
    rgb->format = AV_PIX_FMT_RGBA;
    rgb->width = s.width;
    rgb->height = s.height;
    av_frame_get_buffer(rgb, 0);

    // audio source frame (planar)
    AVFrame* a_src = nullptr;
    if (do_audio) {
        a_src = av_frame_alloc();
        a_src->format = AV_SAMPLE_FMT_FLTP;
        a_src->sample_rate = s.audio_sample_rate;
        av_channel_layout_copy(&a_src->ch_layout, &actx->ch_layout);
        a_src->nb_samples = a_src_samples_ > 0 ? a_src_samples_ : a_frame_size;
        av_frame_get_buffer(a_src, 0);
    }

    // Natural-speed frame budget: the export fps may differ from the timeline
    // fps (Auto resolves to the highest source fps). Both the frame count and
    // every output-frame -> timeline-frame mapping below scale by seq_fps/fps;
    // when they coincide everything reduces to the legacy identity mapping
    // (f -> tl frame f, total_video = s.duration_frames), so existing renders
    // are byte-identical.
    const double seq_fps = project.sequence.fps;
    const double export_fps = s.fps > 0.0 ? s.fps : (seq_fps > 0.0 ? seq_fps : 30.0);
    const double tl_per_frame = seq_fps > 0.0 ? seq_fps / export_fps : 1.0;
    const int64_t total_video = seq_fps > 0.0
        ? (int64_t)std::llround((double)s.duration_frames / seq_fps * export_fps)
        : s.duration_frames;
    const int64_t total_audio = total_video > 0
        ? (int64_t)std::llround((double)total_video / export_fps * s.audio_sample_rate)
        : 0;

    // ---- encode loop ----
    int64_t frame = 0;
    int64_t audio_sample = 0;
    bool ended = false;

    // Always-on per-export telemetry (~1/s lines): frames encoded per second,
    // ETA, the number of non-sequential source decodes, the CPU-vs-GPU fast-path
    // split, composite/audio/encode averages, alloc misses, queue depth, and the
    // GPU fast-path attempt aggregates from frame_gpu() — all in ONE window so
    // the numbers reconcile (render.fast == gpu.landed on a solo-clip GPU
    // export). A render grinding at far below the timeline fps with a big pct
    // gap is composite/decode-bound; stalls climbing between lines mean the
    // compositor keeps reseeking instead of walking frames forward. One instance
    // per export, shared with RenderSession via set_telemetry(), and all note_*
    // calls are thread-safe (producer thread writes, consumer thread ticks).
    canvas::core::log::RenderTelemetry telemetry;
    // Per-export judder-check state (src-frame +1 walk): local + captured, not a
    // static, so a multi-export process never compares frame N of one export to
    // frame M of the previous one.
    int64_t judder_prev_src = INT64_MIN;

    // Decoded audio chunks are per-frame sized (e.g. 800 @48k/60fps) and don't
    // align to the encoder's 1024-sample frame size, so stage them in an
    // accumulator and send only complete encoder frames.
    std::vector<float> a_acc;
    int64_t a_sent = 0;  // encoder frames already fed to aac
    if (do_audio && a_frame_size > 0)
        a_acc.reserve((std::size_t)a_frame_size * 2 * s.audio_channels);

    // Reusable render session: open sources once, reuse decoders, and decode on
    // the GPU for hw exports (removes per-frame avformat_open_input).
    RenderSession session;
    session.set_telemetry(&telemetry);
    bool session_ok = session.begin(project, s.width, s.height, dec_dev);

    // Device-side grade-LUT cache for the fused GPU grade kernel (one upload per
    // graded clip per export). Released before avformat teardown below.
    GpuGradeLut s_gpu_grade;

    // VAAPI encoder-surface feed: the CUDA sibling for AV_PIX_FMT_VAAPI frames.
    // decode_to_hw() hands a VAAPI source; the feed moves it to host NV12
    // (av_hwframe_transfer_data), re-creates the letterbox content rect with an
    // NV12->NV12 swscale when the source isn't canvas-sized, then uploads the
    // full-canvas host frame into a pooled encoder surface (transfer_data again).
    // Fully synchronous — no CUDA events, no device sync — so the pipelined
    // producer treats the result exactly like a CPU frame ({frame, null event,
    // null source}). Only engaged when the encoder's frames format is VAAPI;
    // grades and edge fades bail to the CPU compositor because the fused
    // nv12GradeResize kernel is CUDA-only. All state lives on the producer
    // thread (single-threaded), staging frames are unref'd + refilled each call.
    AVFrame* va_scan = av_frame_alloc();
    AVFrame* va_scaled = av_frame_alloc();
    AVFrame* va_out = av_frame_alloc();
    SwsContext* va_sws = nullptr;
    int va_sws_src_w = 0, va_sws_src_h = 0, va_sws_dst_w = 0, va_sws_dst_h = 0;
    auto vaapi_feed_frame = [&](const int64_t tl) -> AVFrame* {
        RenderSession::GpuFrameInfo gfi;
        const auto t0 = std::chrono::steady_clock::now();
        if (!session.frame_gpu(tl, &gfi) || !gfi.valid) return nullptr;
        // Grade + fade both need the CUDA fused kernel; CPU-composite them so
        // pixels stay identical across encoder backends.
        if (gfi.fade < 1.0f || (gfi.grade && gfi.grade->valid())) return nullptr;
        // NV12 chroma is half-res in both axes: a non-even content offset can't
        // be placed by plane-relative copies without re-subsampling.
        if (gfi.dx < 0 || gfi.dy < 0 || (gfi.dx & 1) || (gfi.dy & 1)) return nullptr;
        if (gfi.dstW <= 0 || gfi.dstH <= 0 || (gfi.dstW & 1) || (gfi.dstH & 1)) return nullptr;
        if (!va_scan || !va_scaled || !va_out) return nullptr;

        // VAAPI surface -> host NV12 at the source's native size. transfer_data
        // stamps the decoded frame's real dims; trust those over gfi.
        av_frame_unref(va_scan);
        va_scan->format = AV_PIX_FMT_NV12;
        if (av_hwframe_transfer_data(va_scan, gfi.source, 0) < 0) return nullptr;
        const int sw = va_scan->width, sh = va_scan->height;
        const bool identity =
            (gfi.dx == 0 && gfi.dy == 0 && sw == s.width && sh == s.height);
        AVFrame* feed = va_scan;
        if (!identity) {
            // Re-create the letterbox rect: scale source -> content size, paste
            // onto a black full-canvas host frame, then upload that whole canvas.
            if (va_sws_src_w != sw || va_sws_src_h != sh ||
                va_sws_dst_w != gfi.dstW || va_sws_dst_h != gfi.dstH) {
                if (va_sws) sws_freeContext(va_sws);
                va_sws = sws_getContext(sw, sh, AV_PIX_FMT_NV12,
                                        gfi.dstW, gfi.dstH, AV_PIX_FMT_NV12,
                                        SWS_BILINEAR, nullptr, nullptr, nullptr);
                va_sws_src_w = sw; va_sws_src_h = sh;
                va_sws_dst_w = gfi.dstW; va_sws_dst_h = gfi.dstH;
            }
            if (!va_sws) return nullptr;
            av_frame_unref(va_scaled);
            va_scaled->format = AV_PIX_FMT_NV12;
            va_scaled->width = gfi.dstW;
            va_scaled->height = gfi.dstH;
            if (av_frame_get_buffer(va_scaled, 32) < 0) return nullptr;
            const uint8_t* srows[] = {va_scan->data[0], va_scan->data[1], nullptr, nullptr};
            const int slines[] = {va_scan->linesize[0], va_scan->linesize[1], 0, 0};
            sws_scale(va_sws, srows, slines, 0, sh, va_scaled->data, va_scaled->linesize);

            av_frame_unref(va_out);
            va_out->format = AV_PIX_FMT_NV12;
            va_out->width = s.width;
            va_out->height = s.height;
            if (av_frame_get_buffer(va_out, 32) < 0) return nullptr;
            for (int r = 0; r < va_out->height; ++r)
                std::memset(va_out->data[0] + (std::size_t)r * (std::size_t)va_out->linesize[0],
                            16, (std::size_t)va_out->linesize[0]);
            for (int r = 0; r < va_out->height / 2; ++r)
                std::memset(va_out->data[1] + (std::size_t)r * (std::size_t)va_out->linesize[1],
                            128, (std::size_t)va_out->linesize[1]);
            for (int r = 0; r < gfi.dstH; ++r)
                std::memcpy(va_out->data[0] +
                                (std::size_t)(gfi.dy + r) * (std::size_t)va_out->linesize[0] + gfi.dx,
                            va_scaled->data[0] + (std::size_t)r * (std::size_t)va_scaled->linesize[0],
                            (std::size_t)gfi.dstW);
            for (int r = 0; r < gfi.dstH / 2; ++r)
                std::memcpy(va_out->data[1] +
                                (std::size_t)(gfi.dy / 2 + r) * (std::size_t)va_out->linesize[1] + gfi.dx,
                            va_scaled->data[1] + (std::size_t)r * (std::size_t)va_scaled->linesize[1],
                            (std::size_t)gfi.dstW);
            feed = va_out;
        }

        AVFrame* hw = av_frame_alloc();
        if (!hw || av_hwframe_get_buffer(hw_frames, hw, 0) != 0) {
            telemetry.note_alloc_miss();
            av_frame_free(&hw);
            return nullptr;
        }
        if (av_hwframe_transfer_data(hw, feed, 0) != 0) {
            telemetry.note_alloc_miss();
            av_frame_free(&hw);
            return nullptr;
        }
        telemetry.note_fast();
        telemetry.note_resize(std::chrono::duration<double, std::milli>(
                                  std::chrono::steady_clock::now() - t0).count());
        return hw;
    };

    // Pipelined render: a producer thread decodes + composites ahead of the
    // main thread, which sends + drains. GPU work overlaps NVENC; a 3-thread
    // split measured no faster — the device serializes decode->kernel->encode.
    const std::size_t producer_depth = 64;
    std::mutex qmu;
    std::condition_variable qcv;
    // Queue slot: AVFrame + optional CUDA event from the async resize stream;
    // the consumer waits on it so the resize completes exactly when NVENC reads it.
    struct ProducerSlot {
        AVFrame* frame = nullptr;
        void* event = nullptr;
        AVFrame* source = nullptr;  // av_frame_ref'd decode source; unref after event wait
    };
    std::deque<ProducerSlot> ready_frames;
    bool producer_done = false;
    auto render_one_frame = [&](const int64_t f) -> std::tuple<AVFrame*, void*, AVFrame*> {
        // Output frame f lands at timeline time f/export_fps; map to the timeline
        // frame shown at that moment (identity when fps == seq fps).
        const int64_t tl = (int64_t)std::llround((double)f * tl_per_frame);
        // GPU fast path: composite a single clip straight into the encoder's
        // CUDA hw frame, skipping the CPU RGBA blit + upload.
        if (enc_is_cuda && session_ok) {
            RenderSession::GpuFrameInfo gfi;
            const bool _gk = session.frame_gpu(tl, &gfi) && gfi.valid;
            if (_gk) {
                telemetry.note_fast();
                // Total frame_gpu attempt cost lives in the renderer-side telemetry
                // (note_gpu_attempt in RenderSession::frame_gpu); no static here.
                // Sanity: output frames must map to strictly advancing source frames;
                // a non-+1 delta means dropped/duplicated frames (judder). Only
                // meaningful at fps == seq fps; otherwise the tl mapping itself
                // duplicates/rounds and the +1 check would false-trip.
                // `judder_prev_src` is captured per-export (declared next to
                // `telemetry`), so it can't bleed across exports in one process.
                if (gfi.src_frame >= 0 && tl_per_frame == 1.0) {
                    if (judder_prev_src != INT64_MIN && gfi.src_frame != judder_prev_src + 1) {
                        telemetry.note_stall();
                        fprintf(stderr, "[FRAME-DIAG] tl_frame=%lld src=+%lld (prev src=%lld) delta=%lld\n",
                                (long long)tl, (long long)gfi.src_frame,
                                (long long)judder_prev_src, (long long)(gfi.src_frame - judder_prev_src));
                    }
                    judder_prev_src = gfi.src_frame;
                }
                AVFrame* hw = av_frame_alloc();
                if (hw && av_hwframe_get_buffer(hw_frames, hw, 0) == 0) {
                    const uintptr_t yc = reinterpret_cast<uintptr_t>(hw->data[0]);
                    const uintptr_t uvc = reinterpret_cast<uintptr_t>(hw->data[1]);
                    auto _tr0 = std::chrono::steady_clock::now();
                    // decode_to_hw() borrows; av_frame_ref() keeps the device planes
                    // alive until the resize + event wait consume them.
                    AVFrame* src_ref = nullptr;
                    if (gfi.source) {
                        src_ref = av_frame_alloc();
                        if (src_ref && av_frame_ref(src_ref, gfi.source) < 0) {
                            av_frame_free(&src_ref);
                            src_ref = nullptr;
                        }
                    }
                    // Async resize on a non-blocking stream; the consumer waits on the event
                    // before sending the frame. Graded clips run the fused grade+resize
                    // kernel (device LUT cached per clip) so they stay on this path.
                    // `src_ref` guards the whole launch: the async kernel must read the
                    // borrowed device planes, which only the av_frame_ref keeps alive.
                    bool resized = src_ref != nullptr;
                    if (resized) {
                        if (gfi.grade && gfi.grade->valid() && s_gpu_grade.ensure(gfi)) {
                            resized = canvas::core::gpu::convert_nv12_grade_resize_async(
                                reinterpret_cast<const uint8_t*>(gfi.srcY),
                                reinterpret_cast<const uint8_t*>(gfi.srcUV),
                                gfi.srcW, gfi.srcH, gfi.srcYPitch, gfi.srcUVPitch,
                                reinterpret_cast<uint8_t*>(yc),
                                static_cast<std::size_t>(hw->linesize[0]),
                                reinterpret_cast<uint8_t*>(uvc),
                                static_cast<std::size_t>(hw->linesize[1]),
                                gfi.outW, gfi.outH, gfi.dstW, gfi.dstH,
                                gfi.dx, gfi.dy, gfi.fade, s_gpu_grade.params(gfi));
                        } else {
                            resized = canvas::core::gpu::convert_nv12_resize_async(
                                reinterpret_cast<const uint8_t*>(gfi.srcY),
                                reinterpret_cast<const uint8_t*>(gfi.srcUV),
                                gfi.srcW, gfi.srcH, gfi.srcYPitch, gfi.srcUVPitch,
                                reinterpret_cast<uint8_t*>(yc),
                                static_cast<std::size_t>(hw->linesize[0]),
                                reinterpret_cast<uint8_t*>(uvc),
                                static_cast<std::size_t>(hw->linesize[1]),
                                gfi.outW, gfi.outH, gfi.dstW, gfi.dstH,
                                gfi.dx, gfi.dy, gfi.fade);
                        }
                    }
                    if (resized) {
                        void* ev = nullptr;
                        canvas::core::gpu::convert_nv12_record_event(&ev);
                        auto _tr1 = std::chrono::steady_clock::now();
                        // Async-launch cost of the resize (the event wait is consumer-side;
                        // the wait axis shows up in the encode time there).
                        telemetry.note_resize(
                            std::chrono::duration<double, std::milli>(_tr1 - _tr0).count());
                        hw->pts = f;
                        return {hw, ev, src_ref};
                    }
                    if (src_ref) av_frame_unref(src_ref);
                } else {
                    telemetry.note_alloc_miss();
                }
                av_frame_free(&hw);
            }
        }

        // VAAPI feed: the encoder's frames are VAAPI surfaces (AMD/Intel, or the
        // dual-vendor case where the machine has CUDA but this export does NOT).
        // Fully synchronous transfer both ways — no CUDA kernels, no events.
        if (enc_is_vaapi && session_ok) {
            if (AVFrame* hw = vaapi_feed_frame(tl)) {
                hw->pts = f;
                return {hw, nullptr, nullptr};
            }
        }

        // CPU composite fallback (identical to the legacy loop): render RGBA on
        // the CPU, then convert to NV12 either with a CUDA kernel or swscale.
        const auto comp_t0 = std::chrono::steady_clock::now();
        auto vf = session_ok ? session.frame(tl)
                             : render_video_frame(project, tl, s.width, s.height, 0);
        if (!vf) return {nullptr, nullptr, nullptr};
        // Live preview (zero-copy: hand the shared_ptr holding the frame we are
        // about to encode; consumer threads marshal it onto the GUI).
        if (preview_throttle.due()) push_preview(vf);
        telemetry.note_cpu(std::chrono::duration<double, std::milli>(
                               std::chrono::steady_clock::now() - comp_t0).count());
        const std::size_t bytes =
            std::min<std::size_t>(vf->rgba.size(), rgb->linesize[0] * (std::size_t)s.height);
        memcpy(rgb->data[0], vf->rgba.data(), bytes);

        AVFrame* to_send = nullptr;

        if (enc_is_cuda) {
            AVFrame* hw = av_frame_alloc();
            if (hw && av_hwframe_get_buffer(hw_frames, hw, 0) == 0) {
                const uintptr_t base = reinterpret_cast<uintptr_t>(hw->data[0]);
                uint8_t* dY = reinterpret_cast<uint8_t*>(base);
                uint8_t* dUV = reinterpret_cast<uint8_t*>(base +
                    static_cast<uintptr_t>(hw->linesize[0]) * static_cast<uintptr_t>(s.height));
                if (canvas::core::gpu::convert_rgba_to_nv12(
                        vf->rgba.data(), vf->width, vf->height,
                        dY, static_cast<std::size_t>(hw->linesize[0]),
                        dUV, static_cast<std::size_t>(hw->linesize[0]),
                        s.width, s.height)) {
                    hw->pts = f;
                    to_send = hw;
                } else {
                    telemetry.note_alloc_miss();
                    av_frame_free(&hw);
                }
            } else {
                telemetry.note_alloc_miss();
                av_frame_free(&hw);
            }
        }

        if (!to_send && v_use_hw && hw_frames) {
            AVFrame* input = av_frame_alloc();
            if (input) {
                input->format = enc_sw_fmt;
                input->width = s.width;
                input->height = s.height;
                if (av_frame_get_buffer(input, 0) >= 0) {
                    const uint8_t* src[] = {rgb->data[0]};
                    int src_lines[] = {rgb->linesize[0]};
                    sws_scale(sws, src, src_lines, 0, s.height, input->data, input->linesize);
                    input->pts = f;
                    AVFrame* hw = av_frame_alloc();
                    if (hw && av_hwframe_get_buffer(hw_frames, hw, 0) == 0) {
                        if (av_hwframe_transfer_data(hw, input, 0) == 0) {
                            hw->pts = f;
                            to_send = hw;
                        } else {
                            av_frame_free(&hw);
                        }
                    } else if (hw) {
                        telemetry.note_alloc_miss();
                        av_frame_free(&hw);
                    }
                }
                av_frame_free(&input);
            }
        }

        if (!to_send && !v_use_hw) {
            AVFrame* input = av_frame_alloc();
            if (input) {
                input->format = enc_sw_fmt;
                input->width = s.width;
                input->height = s.height;
                if (av_frame_get_buffer(input, 0) >= 0) {
                    const uint8_t* src[] = {rgb->data[0]};
                    int src_lines[] = {rgb->linesize[0]};
                    sws_scale(sws, src, src_lines, 0, s.height, input->data, input->linesize);
                    input->pts = f;
                    to_send = input;
                } else {
                    av_frame_free(&input);
                }
            }
        }
        return {to_send, nullptr, nullptr};
    };

    auto producer_thread_fn = [&] {
        int64_t f = 0;
        while (f < total_video) {
            auto [frm, ev, src] = render_one_frame(f);
            // decode_to_hw() borrows its source frame: the decoder recycles the
            // device planes on the next decode call, and av_frame_ref() is only a
            // shallow metadata ref that does NOT keep those planes alive. Failing
            // to wait before decoding f+1 composites stale/garbled NV12 -> spurious
            // pixel jumps the frame counter never catches. So wait on frame f's
            // resize event before decoding f+1 (a no-op if already signaled); the
            // source ref is released here, so the slot carries src=nullptr.
            if (ev) {
                canvas::core::gpu::convert_nv12_wait_event(ev);
                if (src) av_frame_unref(src);
            }
            std::unique_lock<std::mutex> lk(qmu);
            qcv.wait(lk, [&] {
                return cancelled() || ready_frames.size() < producer_depth;
            });
            if (cancelled()) {
                lk.unlock();
                av_frame_free(&frm);
                if (ev) canvas::core::gpu::convert_nv12_destroy_event(ev);
                break;
            }
            ready_frames.push_back({frm, ev, nullptr});  // src already released above
            lk.unlock();
            qcv.notify_all();
            ++f;
        }
        {
            std::lock_guard<std::mutex> lk(qmu);
            producer_done = true;
        }
        qcv.notify_all();
    };

    if (session_ok && total_video > 0 && (enc_is_cuda || enc_is_vaapi)) {
        std::thread producer(producer_thread_fn);

        while (!ended && !cancelled()) {
            AVFrame* to_send = nullptr;
            void* ev = nullptr;
            AVFrame* src = nullptr;
            {
                std::unique_lock<std::mutex> lk(qmu);
                qcv.wait(lk, [&] {
                    return !ready_frames.empty() || producer_done || cancelled();
                });
                if (!ready_frames.empty()) {
                    auto slot = ready_frames.front();
                    ready_frames.pop_front();
                    to_send = slot.frame;
                    ev = slot.event;
                    src = slot.source;
                    telemetry.observe_queue(ready_frames.size());
                }
            }
            qcv.notify_all();

            // Per-event sync so NVENC reads a finished resize; the producer's next
            // decode/resize overlaps this wait + encode.
            if (ev) canvas::core::gpu::convert_nv12_wait_event(ev);
            // Source planes are consumed; drop the ref.
            if (src) av_frame_unref(src);

            const auto enc_t0 = std::chrono::steady_clock::now();
            if (to_send) {
                avcodec_send_frame(vctx, to_send);
                // NVENC reads surfaces asynchronously; barrier before returning the
                // surface to the hw pool, else a recycled in-flight surface
                // duplicates frames. VAAPI feed frames transferred synchronously
                // need no barrier.
                if (enc_is_cuda) canvas::core::gpu::convert_nv12_device_sync();
                av_frame_free(&to_send);
            }
            // One slot per timeline frame; advance in lockstep regardless.
            ++frame;

            // drain output packets from both encoders
            bool got = false;
            AVPacket* pkt = av_packet_alloc();
            while (avcodec_receive_packet(vctx, pkt) == 0) {
                got = true;
                av_packet_rescale_ts(pkt, vctx->time_base, vst->time_base);
                pkt->stream_index = vst->index;
                av_interleaved_write_frame(oc, pkt);
                av_packet_unref(pkt);
            }
            if (do_audio && actx) {
                while (avcodec_receive_packet(actx, pkt) == 0) {
                    got = true;
                    av_packet_rescale_ts(pkt, actx->time_base, ast->time_base);
                    pkt->stream_index = ast->index;
                    av_interleaved_write_frame(oc, pkt);
                    av_packet_unref(pkt);
                }
            }
            av_packet_free(&pkt);
            telemetry.note_encode(
                std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - enc_t0).count());
            telemetry.set_progress(frame, total_video);
            if (canvas::core::gpu::cuda_available())
                telemetry.note_pool_stalls(canvas::core::gpu::nv12_pool_stalls());
            telemetry.tick();

            // audio for this pass
            if (do_audio && audio_sample < total_audio) {
                const int per_frame = (int)std::max<int64_t>(1,
                    (int64_t)std::llround((double)s.audio_sample_rate / s.fps));
                const auto audio_t0 = std::chrono::steady_clock::now();
                auto ac = session.audio_chunk(audio_sample, per_frame,
                                              s.audio_sample_rate, s.audio_channels, s.fps);
                telemetry.note_audio(std::chrono::duration<double, std::milli>(
                                         std::chrono::steady_clock::now() - audio_t0).count());
                const int n = (ac && !ac->samples.empty())
                    ? (int)(ac->samples.size() / s.audio_channels)
                    : 0;
                // Stage samples, then emit complete encoder frames with pts in the
                // encoder's sample domain (exact timing).
                if (n > 0)
                    a_acc.insert(a_acc.end(), ac->samples.begin(), ac->samples.end());
                audio_sample += std::max<int64_t>(n, per_frame);
                const std::size_t ch = (std::size_t)s.audio_channels;
                while (a_acc.size() >= (std::size_t)a_frame_size * ch) {
                    for (std::size_t c = 0; c < ch; ++c)
                        for (int k = 0; k < a_frame_size; ++k)
                            ((float*)a_src->extended_data[c])[k] =
                                a_acc[(std::size_t)k * ch + c];
                    a_src->nb_samples = a_frame_size;
                    a_src->pts = (int64_t)a_frame_size * (int64_t)(a_sent);
                    avcodec_send_frame(actx, a_src);
                    ++a_sent;
                    a_acc.erase(a_acc.begin(), a_acc.begin() + (std::size_t)a_frame_size * ch);
                }
            }

            if (frame >= total_video && (audio_sample >= total_audio || !do_audio)) {
                // flush any staged partial audio frame (tail < a_frame_size)
                if (do_audio && actx && !a_acc.empty()) {
                    const std::size_t ch = (std::size_t)s.audio_channels;
                    const int tail = (int)(a_acc.size() / ch);
                    for (std::size_t c = 0; c < ch; ++c)
                        for (int k = 0; k < tail; ++k)
                            ((float*)a_src->extended_data[c])[k] =
                                a_acc[(std::size_t)k * ch + c];
                    a_src->nb_samples = tail;
                    a_src->pts = (int64_t)a_frame_size * (int64_t)(a_sent);
                    avcodec_send_frame(actx, a_src);
                    ++a_sent;
                    a_acc.clear();
                }
                // flush encoders
                avcodec_send_frame(vctx, nullptr);
                while (true) {
                    AVPacket* p2 = av_packet_alloc();
                    int r = avcodec_receive_packet(vctx, p2);
                    if (r < 0) { av_packet_free(&p2); break; }
                    av_packet_rescale_ts(p2, vctx->time_base, vst->time_base);
                    p2->stream_index = vst->index;
                    av_interleaved_write_frame(oc, p2);
                    av_packet_free(&p2);
                }
                if (do_audio && actx) {
                    avcodec_send_frame(actx, nullptr);
                    while (true) {
                        AVPacket* p2 = av_packet_alloc();
                        int r = avcodec_receive_packet(actx, p2);
                        if (r < 0) { av_packet_free(&p2); break; }
                        av_packet_rescale_ts(p2, actx->time_base, ast->time_base);
                        p2->stream_index = ast->index;
                        av_interleaved_write_frame(oc, p2);
                        av_packet_free(&p2);
                    }
                }
                ended = true;
            }

            if (total_video > 0)
                progress((double)std::min(frame, total_video) / total_video, "Encode");
        }
        // Joined BEFORE the queue/condition-variable/stack teardown below so a
        // producer mid-render_one_frame cannot touch freed state when the export
        // returns (cancel or natural end). The producer exits on cancelled() or
        // after pushing total_video slots, so this join is bounded.
        if (producer.joinable()) producer.join();
        // Free any frames the producer pushed after the loop consumed the last
        // slot (cancel raced the flush); the encoder flush already ran.
        {
            std::lock_guard<std::mutex> lk(qmu);
            while (!ready_frames.empty()) {
                auto slot = ready_frames.front();
                ready_frames.pop_front();
                av_frame_free(&slot.frame);
                if (slot.event) canvas::core::gpu::convert_nv12_destroy_event(slot.event);
                if (slot.source) av_frame_unref(slot.source);
            }
            producer_done = true;
        }
    } else {

    while (!ended && !cancelled()) {
        // Output frame `frame` lands at timeline time; map to the timeline frame
        // (identity when fps == seq fps).
        const int64_t tl = (int64_t)std::llround((double)frame * tl_per_frame);
        if (frame < total_video) {
            // render one frame
            bool gpu_composited = false;

            // GPU fast path: single-clip frames composite on the GPU straight into
            // the encoder's CUDA hw frame, skipping the CPU RGBA blit + full-res
            // upload that dominate software compositing.
            if (enc_is_cuda && session_ok) {
                RenderSession::GpuFrameInfo gfi;
                if (session.frame_gpu(tl, &gfi) && gfi.valid) {
                    telemetry.note_fast();
                    AVFrame* hw = av_frame_alloc();
                    auto tb0 = std::chrono::steady_clock::now();
                    if (hw && av_hwframe_get_buffer(hw_frames, hw, 0) == 0) {
                        const uintptr_t yc = reinterpret_cast<uintptr_t>(hw->data[0]);
                        const uintptr_t uvc = reinterpret_cast<uintptr_t>(hw->data[1]);
                        // Graded clips use the fused grade+resize kernel (device LUT
                        // cached per clip), then sync; ungraded use the sync resize.
                        bool got = false;
                        if (gfi.grade && gfi.grade->valid() && s_gpu_grade.ensure(gfi)) {
                            got = canvas::core::gpu::convert_nv12_grade_resize_async(
                                      reinterpret_cast<const uint8_t*>(gfi.srcY),
                                      reinterpret_cast<const uint8_t*>(gfi.srcUV),
                                      gfi.srcW, gfi.srcH, gfi.srcYPitch, gfi.srcUVPitch,
                                      reinterpret_cast<uint8_t*>(yc),
                                      static_cast<std::size_t>(hw->linesize[0]),
                                      reinterpret_cast<uint8_t*>(uvc),
                                      static_cast<std::size_t>(hw->linesize[1]),
                                      gfi.outW, gfi.outH, gfi.dstW, gfi.dstH,
                                      gfi.dx, gfi.dy, gfi.fade, s_gpu_grade.params(gfi)) &&
                                  canvas::core::gpu::convert_nv12_sync();
                        } else {
                            got = canvas::core::gpu::convert_nv12_resize(
                                reinterpret_cast<const uint8_t*>(gfi.srcY),
                                reinterpret_cast<const uint8_t*>(gfi.srcUV),
                                gfi.srcW, gfi.srcH, gfi.srcYPitch, gfi.srcUVPitch,
                                reinterpret_cast<uint8_t*>(yc),
                                static_cast<std::size_t>(hw->linesize[0]),
                                reinterpret_cast<uint8_t*>(uvc),
                                static_cast<std::size_t>(hw->linesize[1]),
                                gfi.outW, gfi.outH, gfi.dstW, gfi.dstH,
                                gfi.dx, gfi.dy, gfi.fade);
                        }
                        if (got) {
                            hw->pts = frame;
                            avcodec_send_frame(vctx, hw);
                            gpu_composited = true;
                        }
                    }
                    av_frame_free(&hw);
                }
            }

            // VAAPI feed (legacy single-threaded loop): encoder frames are VAAPI
            // surfaces; fully synchronous transfer both ways, no CUDA involved.
            if (!gpu_composited && enc_is_vaapi && session_ok) {
                if (AVFrame* hw = vaapi_feed_frame(tl)) {
                    hw->pts = frame;
                    avcodec_send_frame(vctx, hw);
                    gpu_composited = true;
                    av_frame_free(&hw);
                }
            }

            if (!gpu_composited) {
            const auto comp_t0 = std::chrono::steady_clock::now();
auto vf = session_ok ? session.frame(tl)
                             : render_video_frame(project, tl, s.width, s.height, 0);
            if (vf) {
                // Live preview (zero-copy: the shared_ptr already holds the frame).
                if (preview_throttle.due()) push_preview(vf);
                telemetry.note_cpu(std::chrono::duration<double, std::milli>(
                                       std::chrono::steady_clock::now() - comp_t0).count());
                const std::size_t bytes =
                    std::min<std::size_t>(vf->rgba.size(), rgb->linesize[0] * (std::size_t)s.height);
                memcpy(rgb->data[0], vf->rgba.data(), bytes);

                AVFrame* to_send = nullptr;

                // GPU path: the CUDA kernel writes resized RGBA->NV12 directly into
                // device planes, so NVENC consumes a frame that never left the GPU.
                if (enc_is_cuda) {
                    AVFrame* hw = av_frame_alloc();
                    if (hw && av_hwframe_get_buffer(hw_frames, hw, 0) == 0) {
                        const uintptr_t base = reinterpret_cast<uintptr_t>(hw->data[0]);
                        uint8_t* dY = reinterpret_cast<uint8_t*>(base);
                        uint8_t* dUV = reinterpret_cast<uint8_t*>(base +
                            static_cast<uintptr_t>(hw->linesize[0]) * static_cast<uintptr_t>(s.height));
                        if (canvas::core::gpu::convert_rgba_to_nv12(
                                vf->rgba.data(), vf->width, vf->height,
                                dY, static_cast<std::size_t>(hw->linesize[0]),
                                dUV, static_cast<std::size_t>(hw->linesize[0]),
                                s.width, s.height)) {
                            hw->pts = frame;
                            to_send = hw;
                        } else {
                            telemetry.note_alloc_miss();
                            av_frame_free(&hw);
                        }
                    } else if (hw) {
                        telemetry.note_alloc_miss();
                        av_frame_free(&hw);
                    }
                }

                // CPU fallback: software RGBA->NV12 then upload to the hw frame.
                if (!to_send && v_use_hw && hw_frames) {
                    AVFrame* input = av_frame_alloc();
                    if (input) {
                        input->format = enc_sw_fmt;
                        input->width = s.width;
                        input->height = s.height;
                        if (av_frame_get_buffer(input, 0) >= 0) {
                            const uint8_t* src[] = {rgb->data[0]};
                            int src_lines[] = {rgb->linesize[0]};
                            sws_scale(sws, src, src_lines, 0, s.height, input->data, input->linesize);
                            input->pts = frame;
                            AVFrame* hw = av_frame_alloc();
                            if (hw && av_hwframe_get_buffer(hw_frames, hw, 0) == 0) {
                                if (av_hwframe_transfer_data(hw, input, 0) == 0) {
                                    hw->pts = frame;
                                    to_send = hw;
                                } else {
                                    av_frame_free(&hw);
                                }
                            } else if (hw) {
                                telemetry.note_alloc_miss();
                                av_frame_free(&hw);
                            }
                        }
                        av_frame_free(&input);
                    }
                }

                // Pure software encode (no hw frames): feed the NV12 directly.
                if (!to_send && !v_use_hw) {
                    AVFrame* input = av_frame_alloc();
                    if (input) {
                        input->format = enc_sw_fmt;
                        input->width = s.width;
                        input->height = s.height;
                        if (av_frame_get_buffer(input, 0) >= 0) {
                            const uint8_t* src[] = {rgb->data[0]};
                            int src_lines[] = {rgb->linesize[0]};
                            sws_scale(sws, src, src_lines, 0, s.height, input->data, input->linesize);
                            input->pts = frame;
                            to_send = input;
                            input = nullptr;  // ownership moved to to_send
                        }
                        av_frame_free(&input);
                    }
                }

                if (to_send) {
                    avcodec_send_frame(vctx, to_send);
                    av_frame_free(&to_send);
                }
            }
            }
            ++frame;
        }

        // audio for this pass
        if (do_audio && audio_sample < total_audio) {
            const int per_frame = (int)std::max<int64_t>(1,
                (int64_t)std::llround((double)s.audio_sample_rate / s.fps));
            const auto audio_t0 = std::chrono::steady_clock::now();
            auto ac = session.audio_chunk(audio_sample, per_frame,
                                          s.audio_sample_rate, s.audio_channels, s.fps);
            telemetry.note_audio(std::chrono::duration<double, std::milli>(
                                     std::chrono::steady_clock::now() - audio_t0).count());
            const int n = (ac && !ac->samples.empty())
                ? (int)(ac->samples.size() / s.audio_channels)
                : 0;
            if (n > 0)
                a_acc.insert(a_acc.end(), ac->samples.begin(), ac->samples.end());
            audio_sample += std::max<int64_t>(n, per_frame);
            const std::size_t ch = (std::size_t)s.audio_channels;
            while (a_acc.size() >= (std::size_t)a_frame_size * ch) {
                for (std::size_t c = 0; c < ch; ++c)
                    for (int k = 0; k < a_frame_size; ++k)
                        ((float*)a_src->extended_data[c])[k] =
                            a_acc[(std::size_t)k * ch + c];
                a_src->nb_samples = a_frame_size;
                a_src->pts = (int64_t)a_frame_size * (int64_t)(a_sent);
                avcodec_send_frame(actx, a_src);
                ++a_sent;
                a_acc.erase(a_acc.begin(), a_acc.begin() + (std::size_t)a_frame_size * ch);
            }
        }

        // drain output packets from both encoders
        const auto cpu_enc_t0 = std::chrono::steady_clock::now();
        bool got = false;
        AVPacket* pkt = av_packet_alloc();
        while (avcodec_receive_packet(vctx, pkt) == 0) {
            got = true;
            av_packet_rescale_ts(pkt, vctx->time_base, vst->time_base);
            pkt->stream_index = vst->index;
            av_interleaved_write_frame(oc, pkt);
            av_packet_unref(pkt);
        }
        if (do_audio && actx) {
            while (avcodec_receive_packet(actx, pkt) == 0) {
                got = true;
                av_packet_rescale_ts(pkt, actx->time_base, ast->time_base);
                pkt->stream_index = ast->index;
                av_interleaved_write_frame(oc, pkt);
                av_packet_unref(pkt);
            }
        }
        av_packet_free(&pkt);
        telemetry.note_encode(
            std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - cpu_enc_t0).count());
        telemetry.set_progress(frame, total_video);
        if (canvas::core::gpu::cuda_available())
            telemetry.note_pool_stalls(canvas::core::gpu::nv12_pool_stalls());
        telemetry.tick();

        if (frame >= total_video && (audio_sample >= total_audio || !do_audio)) {
            // flush any staged partial audio frame (tail < a_frame_size)
            if (do_audio && actx && !a_acc.empty()) {
                const std::size_t ch = (std::size_t)s.audio_channels;
                const int tail = (int)(a_acc.size() / ch);
                for (std::size_t c = 0; c < ch; ++c)
                    for (int k = 0; k < tail; ++k)
                        ((float*)a_src->extended_data[c])[k] =
                            a_acc[(std::size_t)k * ch + c];
                a_src->nb_samples = tail;
                a_src->pts = (int64_t)a_frame_size * (int64_t)(a_sent);
                avcodec_send_frame(actx, a_src);
                ++a_sent;
                a_acc.clear();
            }
            // flush encoders
            avcodec_send_frame(vctx, nullptr);
            while (true) {
                AVPacket* p2 = av_packet_alloc();
                int r = avcodec_receive_packet(vctx, p2);
                if (r < 0) { av_packet_free(&p2); break; }
                av_packet_rescale_ts(p2, vctx->time_base, vst->time_base);
                p2->stream_index = vst->index;
                av_interleaved_write_frame(oc, p2);
                av_packet_free(&p2);
            }
            if (do_audio && actx) {
                avcodec_send_frame(actx, nullptr);
                while (true) {
                    AVPacket* p2 = av_packet_alloc();
                    int r = avcodec_receive_packet(actx, p2);
                    if (r < 0) { av_packet_free(&p2); break; }
                    av_packet_rescale_ts(p2, actx->time_base, ast->time_base);
                    p2->stream_index = ast->index;
                    av_interleaved_write_frame(oc, p2);
                    av_packet_free(&p2);
                }
            }
            ended = true;
        }

        if (total_video > 0)
            progress((double)std::min(frame, total_video) / total_video, "Encode");
    }

    }  // else: legacy single-threaded loop

    av_write_trailer(oc);

    // Flush whatever the telemetry window still holds (short exports finish with
    // under a second of data; the 1/s cadence never fired). Also re-arms the
    // window so the same process can export again without stale counters.
    telemetry.flush();

    CANVAS_LOG("render: complete out='%s' frames=%lld audio_samples=%lld",
           s.output_path.c_str(), (long long)frame, (long long)audio_sample);

    // Free the device-side grade LUT cache. Both encode paths have finished and
    // synced by now (producer joined + event-waited; else path convert_nv12_sync
    // called per frame), so no queued kernel can still read the LUT. Safe to
    // call with a null cache.
    s_gpu_grade.release();

    if (a_src) av_frame_free(&a_src);
    av_frame_free(&rgb);
    if (va_sws) sws_freeContext(va_sws);
    av_frame_free(&va_out);
    av_frame_free(&va_scaled);
    av_frame_free(&va_scan);
    if (sws) sws_freeContext(sws);
    avcodec_free_context(&actx);
    avcodec_free_context(&vctx);
    if (hw_frames) av_buffer_unref(&hw_frames);
    if (dec_dev) av_buffer_unref(&dec_dev);
    if (oc && oc->pb) avio_closep(&oc->pb);
    avformat_free_context(oc);

    if (cancelled()) return fail("Export cancelled.");
    return true;
}

}  // namespace canvas::core
