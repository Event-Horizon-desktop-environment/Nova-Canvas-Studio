#include "canvas/core/export/exporter.hpp"

#include "canvas/core/export/renderer.hpp"
#include "canvas/core/gpu/cuda_convert.hpp"
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
                // A hardware-frame format: prefer it when present so device
                // encoders (NVENC/VAAPI/QSV) are fed device frames instead of
                // running in host-memory mode. NVENC lists YUV420P/NV12 before
                // CUDA, so we must scan the whole list and not short-circuit on
                // the first software format.
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

// Translates the X264-style preset names shown in the Deliver UI into the
// preset strings each encoder family actually accepts.
//
//  - NVENC/VAAPI/QSV/AMF (hardware): reject x264 names like "faster" or
//    "placebo" with "Undefined constant or missing '(' in 'faster'", so map them
//    to the p1..p7 NVENC presets.
//  - SVT-AV1 (software): same x264-name rejection; it uses a numeric 0..13
//    preset where HIGHER = faster, so reverse the x264 ordering.
//  - libx264/libx265 (software): keep the verbatim x264 preset name.
std::string nv_preset_for(const std::string& codec, const std::string& preset) {
    const std::string p = [&] {
        std::string q = preset;
        for (auto& c : q) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return q;
    }();

    // SVT-AV1: numeric preset, higher = faster. Map x264 order (placebo slowest
    // .. ultrafast fastest) onto SVT's 0..13 range.
    if (codec.find("svt") != std::string::npos || codec.find("av1") != std::string::npos) {
        // Software AV1 encoders that still use x264-style names (libaom) accept
        // 0..9 as well (higher = faster); keep the same inverted mapping.
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

    // Alignment note (Resolve parity): DaVinci Resolve's "Faster" audio/video
    // preset maps to NVENC speed preset p2, not p4. Verified by decoding Resolve's
    // encoder_command_param_map (preset=faster on a completed 44520-frame 1440p60
    // HEVC 80 Mbps CBR render that sustained ~700 fps, the same throughput our
    // exporter measures at NVENC p2). Keep that pairing so a "Faster" export here
    // runs at the same speed/quality as Resolve's.
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

bool export_project(const Project& project, const ExportSettings& s, ExportControl* control,
                    std::string* error) {
    const auto fail = [&](const std::string& m) {
        // Always record the failure reason so render errors are never silently
        // swallowed by the CANVAS_DEBUG gate (render failures are the #1 debugging
        // target). Writes to stderr + the log file unconditionally.
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

    const AVCodec* vcodec = avcodec_find_encoder_by_name(s.video_codec.c_str());
    if (!vcodec) { avformat_free_context(oc); return fail("Unknown video encoder: " + s.video_codec); }

    AVCodecContext* vctx = avcodec_alloc_context3(vcodec);
    if (!vctx) { avformat_free_context(oc); return fail("No video codec context."); }

    const std::string hw_device = hw_device_for_codec(s.video_codec);
    const bool hw_codec = !hw_device.empty();
    bool v_use_hw = false;
    const AVPixelFormat hw_pix = pick_video_fmt(vcodec, &v_use_hw);
    v_use_hw = v_use_hw && hw_codec;

    // Pixel format the (software) encoder consumes:
    //  - Hardware encoders (NVENC/VAAPI/QSV) are fed NV12 then uploaded.
    //  - Software encoders honor the codec's preferred sw format (e.g. ProRes
    //    yuv422p10le), falling back to YUV420P. We must NOT blindly force
    //    YUV420P or encoders that only accept other formats fail to open.
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
    // Color metadata: the render path composites in the source's native 8-bit
    // limited-range bt709 space (no color conversion is applied), so the export
    // must declare bt709 + limited (TV) range or the muxed file comes out with
    // `unknown` color tags. Resolve/other NLEs stamp bt709 here; missing tags
    // force players/GPU pipelines to guess the transfer, often sliding into a
    // slow per-frame software colorspace conversion that reads as stutter.
    vctx->color_range = AVCOL_RANGE_MPEG;          // limited / TV range
    vctx->colorspace = AVCOL_SPC_BT709;            // BT.709 primaries
    vctx->color_trc = AVCOL_TRC_BT709;             // BT.709 transfer
    vctx->color_primaries = AVCOL_PRI_BT709;       // BT.709 primaries
    // NOTE: HEVC level is deliberately NOT set via `vctx->level` — NVENC ignores
    // the context field and uses its own private `level` option (which defaults
    // to "auto" and under-picks to Main@3.1 for 1440p60). The correct private
    // option is set further down alongside the other NVENC tuning options.
    // Emit SPS/PPS (or equivalent) into vctx->extradata at open time so
    // avcodec_parameters_from_context() picks them up. Without this the header
    // extradata is empty until the first keyframe, which makes muxers that write
    // codec-private data at header time (matroska, mxf, ...) reject H.264/H.265.
    vctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

    // Set crf for quality-driven modes (crf >= 0; "Best" uses 0). Two encoders
    // model constant quality through QP/ICQ; crf 0 is a valid "highest quality"
    // request, but many also interpret a *set* bit_rate as overriding it, so the
    // two stays are kept mutually exclusive below (crf wins).
    if (s.crf >= 0) av_opt_set_int(vctx->priv_data, "crf", s.crf, 0);
    // Bitrate-driven mode: pick a target rate only when one is specified AND no
    // crf is active. Never combine a bitrate with crf — constant-quality encoders
    // (SVT-AV1 in particular) reject having BOTH set, and crf would be shadowed
    // anyway. Quality modes (ConstantQP / VBRQuality) carry video_bitrate_kbps==0,
    // so they land here as no-op regardless of crf value.
    const bool is_nvenc = s.video_codec.find("nvenc") != std::string::npos;
    if (s.video_bitrate_kbps > 0 && s.crf < 0) {
        const int64_t bps = static_cast<int64_t>(s.video_bitrate_kbps) * 1000;
        vctx->bit_rate = bps;
        // True CBR: a tight VBV window forces each segment to stay at the target
        // average. NVENC additionally needs `rc` spelled out or it silently falls
        // back to a low-default bitrate (the ~11 Mbps the user saw), ignoring the
        // target entirely.
        const bool cbr = s.vid_rc_mode == "cbr";
        const bool vbr = s.vid_rc_mode == "vbr_target";
        if (cbr || vbr) {
            const int64_t max_bps =
                s.video_max_bitrate_kbps > 0
                    ? static_cast<int64_t>(s.video_max_bitrate_kbps) * 1000
                    : bps;
            // VBV window: tight (== target) for CBR so the rate is held; looser
            // (2× max) for VBR so quality can peak but still be bounded.
            const int64_t bufsize_bps = cbr ? max_bps : max_bps * 2;
            av_opt_set_int(vctx, "maxrate", max_bps, AV_OPT_SEARCH_CHILDREN);
            av_opt_set_int(vctx, "bufsize", bufsize_bps, AV_OPT_SEARCH_CHILDREN);
            if (is_nvenc)
                av_opt_set(vctx->priv_data, "rc", cbr ? "cbr" : "vbr", 0);
        }
    }
    // NVENC constant-QP mode: set the encoder's `rc` and `cq`/`qp` so it actually
    // uses the quality setting instead of defaulting to a fixed 8-12 Mbps ABR that
    // looks terrible on 80 Mbps-scale work.
    if (is_nvenc && s.crf >= 0 && s.vid_rc_mode == "constqp") {
        av_opt_set(vctx->priv_data, "rc", "constqp", 0);
        av_opt_set_int(vctx->priv_data, "cq", s.crf, 0);
    }
    if (!s.preset.empty())
        av_opt_set(vctx->priv_data, "preset", nv_preset_for(s.video_codec, s.preset).c_str(), 0);
    apply_codec_extra(vctx, s.extra);

    // Split-frame encoding (SFE): on GPUs with multiple NVENC engines (the RTX
    // 5070 Ti has two), the driver splits each frame into horizontal strips and
    // the engines encode them in parallel on a single session/stream. Exposed by
    // FFmpeg (7.1+) as `split_encode_mode`; 1 = `forced` (driver picks the strip
    // count for however many engines are present) vs 2 = hardcoded two-way.
    // `forced` is preferred for portability so the same setting cleanly works on
    // single-engine (5070), dual-engine (5070 Ti/5080) and triple-engine (5090)
    // parts. NVIDIA restricts SFE to HEVC/AV1 (H.264 exposes no such option), so
    // only set it for those codecs; `av_opt_set_int` silently no-ops when the
    // encoder doesn't expose the option.
    const std::string vc = s.video_codec;
    if (vc.find("nvenc") != std::string::npos &&
        (vc.find("av1") != std::string::npos || vc.find("hevc") != std::string::npos ||
         vc.find("h265") != std::string::npos)) {
        av_opt_set_int(vctx->priv_data, "split_encode_mode", 1, 0);
    }
    // HEVC level: NVENC's default "auto" under-picks to Main@3.1 for 1440p60,
    // which is a spec-invalid combination (level 3.1 caps at 1080p). Decoders
    // that validate against the declared level then bail out of the hardware
    // path into slow software decode -> "duplicated-motion" judder on playback.
    // Stamp the highest level the output dimensions/fps require so the stream
    // honestly advertises its headroom. Values are NVENC's H.264-style level
    // integers (150 = 3.1 ... 183 = 5.0 ... 186 = 5.1).
    if (is_nvenc && (vc.find("hevc") != std::string::npos ||
                     vc.find("h265") != std::string::npos)) {
        // Map the output's luma sample rate to HEVC Main-level capability so the
        // stream honestly advertises decode headroom. NVENC's "auto" under-picks
        // to Main@3.1 for 1440p60 (level 3.1 caps at 1080p), which makes strict
        // decoders drop to slow software decode -> "duplicated-motion" judder.
        // Values are NVENC's H.264-style level integers (150=3.1 ... 183=5.0).
        const double luma_sps =
            static_cast<double>(s.width) * static_cast<double>(s.height) * s.fps;
        int level = 183;  // Main@5.0: up to 2.56 Gsamples/s (covers 1440p60 @ 0.22G)
        if (luma_sps > 2.56e9) level = 186;   // 5.1
        if (luma_sps > 3.07e9) level = 200;   // 6.0
        av_opt_set_int(vctx->priv_data, "level", level, 0);
    }
    // Allow the encoder to keep several frames in flight so avcodec_send_frame
    // does not stall behind a too-small surface pool at high throughput.
    //
    // NVIDIA's NVENC needs the surface pool to be large enough for the requested
    // lookahead: it requires roughly `rc_lookahead + (max_b_frames) + 8` surfaces.
    // The lookahead arrives via the extra options ("rc-lookahead=N" in `s.extra`),
    // and if we hardcode a too-small surfaces value FFmpeg has to bump it up at
    // open time (the "Defined rc_lookahead requires more surfaces" log). Derive a
    // surfaces count from whatever rc-lookahead is requested so that never happens.
    if (v_use_hw) {
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
        // NVENC needs roughly `rc_lookahead + max_b_frames + 8` surfaces. Add
        // the B-frame depth on top of the lookahead window. The encoder uses
        // max_b_frames=0 (progressive, matching the reference clips that play
        // smooth), so no B-frame margin is required beyond the base pool.
        const int surfaces = rc_lookahead > 0 ? rc_lookahead + 10 : 12;
        av_opt_set_int(vctx->priv_data, "surfaces", surfaces, 0);
    }

    ::AVBufferRef* hw_frames = nullptr;
    ::AVBufferRef* dec_dev = nullptr;  // device used for accelerated decode of sources
    if (v_use_hw) {
        const AVHWDeviceType dt = av_hwdevice_find_type_by_name(hw_device.c_str());
        ::AVBufferRef* dev = nullptr;
        if (dt != AV_HWDEVICE_TYPE_NONE)
            av_hwdevice_ctx_create(&dev, dt, nullptr, nullptr, 0);
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
            // NOTE: keep the device referenced via `dec_dev` (aliased to `dev`).
            // `av_hwframe_ctx_alloc`/`av_hwframe_ctx_init` hold their own device
            // reference, so unref'ing `dev` here would drop the device refcount
            // to zero and leave the decoders' `dec_dev` dangling once the frames
            // context is gone. `dec_dev` is released at teardown.
        }
        if (!hw_frames) v_use_hw = false;
    }

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
    // Frame-rate metadata: some muxers (mpeg, matroska, ...) require an explicit
    // frame rate on the stream or refuse/skew the written header.
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
        // The per-timeline-frame audio payload can exceed the encoder's frame
        // size (e.g. 96kHz/60fps => 1600 samples > aac's 1024).  a_src must be
        // sized for the larger of the two or feeding nb_samples = per_frame
        // would read past the allocated planes.
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

    // The software pixel format the encoder consumes. For hardware encoders we
    // feed NV12 from swscale and upload to the hw frames; for software encoders
    // we feed exactly vctx->pix_fmt (e.g. YUV420P for libx264).
    const AVPixelFormat enc_sw_fmt = v_use_hw ? AV_PIX_FMT_NV12 : vctx->pix_fmt;
    SwsContext* sws = sws_getContext(s.width, s.height, AV_PIX_FMT_RGBA,
                                     s.width, s.height, enc_sw_fmt,
                                     SWS_BILINEAR, nullptr, nullptr, nullptr);

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

    const int64_t total_video = s.duration_frames;
    const int64_t total_audio = total_video > 0
        ? (int64_t)std::llround((double)total_video / s.fps * s.audio_sample_rate)
        : 0;

    // ---- encode loop ----
    int64_t frame = 0;
    int64_t audio_sample = 0;
    bool ended = false;

    // Audio is fed to the encoder in exact aac frame_size (1024-sample) chunks.
    // Decoded chunks are per_video_frame-sized (e.g. 800 @48k/60fps) and do not
    // align to 1024, so a persistent accumulator stages them and only complete
    // encoder frames are sent. count is the number of interleaved floats staged,
    // i.e. a_src->nb_samples*channels when drained.
    std::vector<float> a_acc;
    int64_t a_sent = 0;  // encoder frames already fed to aac
    if (do_audio && a_frame_size > 0)
        a_acc.reserve((std::size_t)a_frame_size * 2 * s.audio_channels);

    // Reusable render session: opens each source once and reuses the decoders
    // across frames, and (for hardware exports) decodes sources on the GPU. This
    // removes the per-frame avformat_open_input that dominated software timing.
    RenderSession session;
    bool session_ok = session.begin(project, s.width, s.height, dec_dev);

    // Pipelined render: a producer thread decodes + composits encoder-ready
    // frames (GPU fast path: NVDEC -> nv12Resize straight into a CUDA hw frame)
    // ahead of the main thread, which only sends + drains. The producer's GPU
    // work overlaps NVENC encode on the main thread, and the pure-composite
    // experiment (3 threads: decode / composite / encode) measured no faster —
    // the device serializes the decode->kernel->encode chain regardless.
    const std::size_t producer_depth = 64;
    std::mutex qmu;
    std::condition_variable qcv;
    // Each queue slot carries the AVFrame + an optional CUDA event handle from
    // the async resize stream.  The consumer waits on the event before feeding
    // the frame to NVENC so the resize kernel completes exactly when needed,
    // without a full-device sync in the producer.
    struct ProducerSlot {
        AVFrame* frame = nullptr;
        void* event = nullptr;
        AVFrame* source = nullptr;  // av_frame_ref'd decode source; unref after event wait
    };
    std::deque<ProducerSlot> ready_frames;
    bool producer_done = false;
    auto render_one_frame = [&](const int64_t f) -> std::tuple<AVFrame*, void*, AVFrame*> {
        // GPU composite fast path: single enabled clip decoded to GPU NV12 ->
        // composite (letterbox + resize) entirely on the GPU into the encoder's
        // CUDA hw frame. Skips the CPU full-res RGBA canvas blit + upload.
        if (v_use_hw && hw_frames && session_ok &&
            canvas::core::gpu::cuda_available()) {
            RenderSession::GpuFrameInfo gfi;
            auto _tf0 = std::chrono::steady_clock::now();
            const bool _gk = session.frame_gpu(f, &gfi) && gfi.valid;
            auto _tf1 = std::chrono::steady_clock::now();
            static double _st_fg = 0, _st_rz = 0; static long _cnt = 0;
            if (_gk) {
                _st_fg += std::chrono::duration<double, std::milli>(_tf1 - _tf0).count();
                // Sanity: consecutive output frames must map to a strictly
                // advancing source frame.  A non-+1 delta means the decoder
                // overshot (dropped frames) or repeated (duplicate frames),
                // which would manifest as judder/dup-frames in the output.
                static int64_t s_prev_src = INT64_MIN;
                if (gfi.src_frame >= 0) {
                    if (s_prev_src != INT64_MIN && gfi.src_frame != s_prev_src + 1)
                        fprintf(stderr, "[FRAME-DIAG] tl_frame=%lld src=+%lld (prev src=%lld) delta=%lld\n",
                                (long long)f, (long long)gfi.src_frame,
                                (long long)s_prev_src, (long long)(gfi.src_frame - s_prev_src));
                    s_prev_src = gfi.src_frame;
                }
                AVFrame* hw = av_frame_alloc();
                if (hw && av_hwframe_get_buffer(hw_frames, hw, 0) == 0) {
                    const uintptr_t yc = reinterpret_cast<uintptr_t>(hw->data[0]);
                    const uintptr_t uvc = reinterpret_cast<uintptr_t>(hw->data[1]);
                    auto _tr0 = std::chrono::steady_clock::now();
                    // The decode_to_hw() result is a borrowed frame the decoder
                    // recycles on the next call.  av_frame_ref() it so the device
                    // planes stay alive until the resize kernel (and its event
                    // wait) have consumed them.  The ref is released in the
                    // consumer after convert_nv12_wait_event().
                    AVFrame* src_ref = nullptr;
                    if (gfi.source) {
                        src_ref = av_frame_alloc();
                        if (src_ref && av_frame_ref(src_ref, gfi.source) < 0) {
                            av_frame_free(&src_ref);
                            src_ref = nullptr;
                        }
                    }
                    // Async resize: kernel launches on a non-blocking stream so
                    // the producer can start the next decode immediately.  The
                    // consumer waits on the event before avcodec_send_frame.
                    if (src_ref && canvas::core::gpu::convert_nv12_resize_async(
                            reinterpret_cast<const uint8_t*>(gfi.srcY),
                            reinterpret_cast<const uint8_t*>(gfi.srcUV),
                            gfi.srcW, gfi.srcH, gfi.srcYPitch, gfi.srcUVPitch,
                            reinterpret_cast<uint8_t*>(yc),
                            static_cast<std::size_t>(hw->linesize[0]),
                            reinterpret_cast<uint8_t*>(uvc),
                            static_cast<std::size_t>(hw->linesize[1]),
                            gfi.outW, gfi.outH, gfi.dstW, gfi.dstH,
                            gfi.dx, gfi.dy)) {
                        void* ev = nullptr;
                        canvas::core::gpu::convert_nv12_record_event(&ev);
                        auto _tr1 = std::chrono::steady_clock::now();
                        _st_rz += std::chrono::duration<double, std::milli>(_tr1 - _tr0).count();
                        if (++_cnt % 90 == 0)
                            fprintf(stderr, "[TIMING] frame=%lld fastpath: frame_gpu=%.3fms resize=%.3fms (avg over %ld)\n",
                                    (long long)f, _st_fg / _cnt, _st_rz / _cnt, _cnt);
                        hw->pts = f;
                        return {hw, ev, src_ref};
                    }
                    if (src_ref) av_frame_unref(src_ref);
                }
                av_frame_free(&hw);
            }
        }

        // CPU composite fallback (identical to the legacy loop): render RGBA on
        // the CPU, then convert to NV12 either with a CUDA kernel or swscale.
        auto vf = session_ok ? session.frame(f)
                             : render_video_frame(project, f, s.width, s.height, 0);
        if (!vf) return {nullptr, nullptr, nullptr};
        const std::size_t bytes =
            std::min<std::size_t>(vf->rgba.size(), rgb->linesize[0] * (std::size_t)s.height);
        memcpy(rgb->data[0], vf->rgba.data(), bytes);

        AVFrame* to_send = nullptr;

        if (v_use_hw && hw_frames && canvas::core::gpu::cuda_available()) {
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
                    av_frame_free(&hw);
                }
            } else if (hw) {
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
            // CRITICAL: `decode_to_hw` returns a BORROWED source frame whose
            // device planes the decoder recycles on the very next decode call.
            // The async resize kernel reads those planes; `av_frame_ref(src)` is
            // only a shallow metadata ref and does NOT keep the decoder's device
            // planes alive. If the producer decodes frame f+1 before frame f's
            // resize has read the planes, the resize composites stale/garbled
            // NV12 -> intermittent spurious pixel jumps ("A holds, holds, B
            // jumps") that manifest as judder even though [FRAME-DIAG] (which
            // tracks the frame-number counter, not pixels) stays silent.
            //
            // Fix: wait for frame f's async resize to finish reading its source
            // BEFORE decoding f+1 (which would recycle the planes). Waiting on an
            // already-signaled event is a no-op, so the consumer's later wait of
            // the same event is still correct. The source ref is released here,
            // so the slot hands the consumer {frm, ev, src=nullptr} and the
            // consumer's av_frame_unref(src) becomes a no-op.
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

    if (session_ok && total_video > 0 &&
        v_use_hw && hw_frames && canvas::core::gpu::cuda_available()) {
        std::thread producer(producer_thread_fn);
        producer.detach();

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
                }
            }
            qcv.notify_all();

            // Wait for the async resize kernel to finish before feeding the
            // frame to NVENC.  This is a per-event sync (not a full device
            // sync), so the producer's next decode/resize can overlap with
            // this wait + the encode.
            if (ev) canvas::core::gpu::convert_nv12_wait_event(ev);
            // Release the av_frame_ref'd decode source now that its device
            // planes are no longer needed by the resize kernel.
            if (src) av_frame_unref(src);

            if (to_send) {
                avcodec_send_frame(vctx, to_send);
                // NVENC reads the input surface asynchronously on the device.
                // Barrier here so the surface is fully consumed before it is
                // returned to the hw pool below; otherwise the producer can
                // recycle an in-flight surface and the encode emits duplicate/
                // repeating frames.
                canvas::core::gpu::convert_nv12_device_sync();
                av_frame_free(&to_send);
            }
            // The producer emits exactly one slot per timeline frame (nullptr when
            // nothing was drawn), so advance in lockstep regardless of to_send.
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

            // audio for this pass
            if (do_audio && audio_sample < total_audio) {
                const int per_frame = (int)std::max<int64_t>(1,
                    (int64_t)std::llround((double)s.audio_sample_rate / s.fps));
                auto ac = session.audio_chunk(audio_sample, per_frame,
                                              s.audio_sample_rate, s.audio_channels, s.fps);
                const int n = (ac && !ac->samples.empty())
                    ? (int)(ac->samples.size() / s.audio_channels)
                    : 0;
                // Stage decoded samples into the accumulator, then push complete
                // aac frames (a_frame_size) with pts in the encoder's own sample
                // domain so timing is exact and no "frame_size not respected"
                // warning can occur.
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
        if (frame < total_video) {
            // render one frame
            bool gpu_composited = false;

            // GPU composite fast path: when the frame is a single enabled clip
            // decoded to GPU NV12, composite (letterbox + resize) entirely on
            // the GPU straight into the encoder's CUDA hw frame. This skips the
            // CPU full-res RGBA canvas blit and the full-res CPU->GPU upload
            // that dominate software compositing in single-clip exports.
            if (v_use_hw && hw_frames && session_ok &&
                canvas::core::gpu::cuda_available()) {
                RenderSession::GpuFrameInfo gfi;
                if (session.frame_gpu(frame, &gfi) && gfi.valid) {
                    AVFrame* hw = av_frame_alloc();
                    auto tb0 = std::chrono::steady_clock::now();
                    if (hw && av_hwframe_get_buffer(hw_frames, hw, 0) == 0) {
                        const uintptr_t yc = reinterpret_cast<uintptr_t>(hw->data[0]);
                        const uintptr_t uvc = reinterpret_cast<uintptr_t>(hw->data[1]);
                        if (canvas::core::gpu::convert_nv12_resize(
                                reinterpret_cast<const uint8_t*>(gfi.srcY),
                                reinterpret_cast<const uint8_t*>(gfi.srcUV),
                                gfi.srcW, gfi.srcH, gfi.srcYPitch, gfi.srcUVPitch,
                                reinterpret_cast<uint8_t*>(yc),
                                static_cast<std::size_t>(hw->linesize[0]),
                                reinterpret_cast<uint8_t*>(uvc),
                                static_cast<std::size_t>(hw->linesize[1]),
                                gfi.outW, gfi.outH, gfi.dstW, gfi.dstH,
                                gfi.dx, gfi.dy)) {
                            hw->pts = frame;
                            avcodec_send_frame(vctx, hw);
                            gpu_composited = true;
                        }
                    }
                    av_frame_free(&hw);
                }
            }

            if (!gpu_composited) {
            auto vf = session_ok ? session.frame(frame)
                                 : render_video_frame(project, frame, s.width, s.height, 0);
            if (vf) {
                const std::size_t bytes =
                    std::min<std::size_t>(vf->rgba.size(), rgb->linesize[0] * (std::size_t)s.height);
                memcpy(rgb->data[0], vf->rgba.data(), bytes);

                AVFrame* to_send = nullptr;

                // GPU path: skip the CPU sws_scale + upload entirely. Allocate a
                // CUDA hw frame and have the CUDA kernel write the resized
                // RGBA->NV12 result directly into its device planes, so NVENC
                // consumes a frame that never left the GPU for conversion.
                if (v_use_hw && hw_frames && canvas::core::gpu::cuda_available()) {
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
                            av_frame_free(&hw);
                        }
                    } else if (hw) {
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
            auto ac = session.audio_chunk(audio_sample, per_frame,
                                          s.audio_sample_rate, s.audio_channels, s.fps);
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

    CANVAS_LOG("render: complete out='%s' frames=%lld audio_samples=%lld",
           s.output_path.c_str(), (long long)frame, (long long)audio_sample);

    if (a_src) av_frame_free(&a_src);
    av_frame_free(&rgb);
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
