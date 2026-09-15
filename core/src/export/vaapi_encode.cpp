#include "canvas/core/export/vaapi_encode.hpp"

#include "canvas/core/util/log.hpp"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/opt.h>
}

#include <string>

namespace canvas::core {

namespace {

// AV1 VAAPI consumes a 0-255 quality scale, while H.264/HEVC vaapi use the
// 0-51 x264-like scale. OBS and mainstream hardware-encode deployments feed
// crf*5 for AV1 (crf already sits in 0-51 space), which maps the two scales.
bool is_av1(const std::string& codec_name) {
    return codec_name.find("av1") != std::string::npos;
}

bool is_hevc(const std::string& codec_name) {
    return codec_name.find("hevc") != std::string::npos ||
           codec_name.find("h265") != std::string::npos;
}

}  // namespace

bool is_vaapi_codec(const std::string& codec_name) {
    return codec_name.find("vaapi") != std::string::npos;
}

VaapiRateControl vaapi_rate_control_from(const std::string& codec_name, int crf,
                                         const std::string& vid_rc_mode,
                                         int video_bitrate_kbps) {
    VaapiRateControl rc;
    if (!is_vaapi_codec(codec_name)) return rc;

    // Quality-driven (crf >= 0): the exporter keeps crf and bit_rate mutually
    // exclusive, so a usable bitrate can't coexist here — these are the modes
    // that reach the encoder with bit_rate unset.
    //
    // radeonsi (AMD Mesa): h264_vaapi supports CQP, CBR, VBR, QVBR — no ICQ.
    // hevc_vaapi supports the same set. QVBR requires a bitrate to function
    // (it's a quality-targeted VBR, not pure constant-quality). CQP is the
    // only universal constant-quality mode across all VAAPI encoders on AMD.
    if (crf >= 0) {
        const int scale = is_av1(codec_name) ? 5 : 1;
        if (vid_rc_mode == "constqp") {
            rc.rc_mode = "CQP";          // strict constant-QP (the "qp" knob)
            rc.qp = crf * scale;
            rc.global_quality = crf * scale;
        } else if (vid_rc_mode == "vbr" || vid_rc_mode == "vbr_target") {
            // QVBR is quality-targeted VBR; it requires a bitrate budget.
            // When only quality is provided (no bitrate), fall back to CQP
            // which gives constant-quality encoding without a bitrate.
            if (video_bitrate_kbps > 0 && (is_hevc(codec_name) || is_av1(codec_name))) {
                rc.rc_mode = "QVBR";
            } else {
                rc.rc_mode = "CQP";      // h264 has no ICQ; universal fallback
                rc.qp = crf * scale;
            }
            rc.global_quality = crf * scale;
        } else {                         // "auto" (or unspecified)
            // CQP: universally supported constant-quality mode. h264_vaapi on
            // AMD radeonsi has no ICQ; hevc/av1 could use QVBR but that needs
            // a bitrate which isn't available in the pure-quality path.
            rc.rc_mode = "CQP";
            rc.qp = crf * scale;
            rc.global_quality = crf * scale;
        }
        return rc;
    }

    // Bitrate-driven (crf < 0): rc_mode communicates the intent; the exporter
    // already wires bit_rate + maxrate/bufsize (VBV) for these modes. Accept
    // any "vbr-ish" spelling, including the app's "vbr_target".
    if (vid_rc_mode == "cbr") {
        rc.rc_mode = "CBR";
    } else if (vid_rc_mode == "vbr" || vid_rc_mode == "vbr_target") {
        rc.rc_mode = "VBR";
    }
    return rc;
}

VaapiSpeed vaapi_speed_for(const std::string& codec_name, const std::string& preset) {
    bool h264 = codec_name.find("h264") != std::string::npos;
    VaapiSpeed sp;

    std::string p = preset;
    for (auto& c : p) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

    // Encoder defaults are already the fastest measured point on radeonsi
    // (async_depth=2 saturates VCN; quality=-1 = fastest). Keep the whole fast
    // half on defaults and only trade speed for quality at the slow end, where
    // the h264 `quality` option's inverted law (higher = slower) is honored.
    // async_depth only dips on the genuinely deliberate presets — a minimum of
    // 1 is the floor the option accepts, but we stay at 2 (the default driver
    // floor for parallel VCN pipelines) rather than 1, which was measured no
    // faster and risks a serial encode on some kernels.
    if (p == "ultrafast" || p == "superfast" || p == "veryfast" ||
        p == "fast" || p == "faster" || p.empty()) {
        return sp;  // defaults: async_depth 2, quality -1 (the fastest row)
    }
    if (p == "medium") {
        if (h264) sp.quality = -1;  // same speed as fast; quality untouched
        return sp;
    }
    // Slow / very slow / placebo: buy quality with speed. h264 `quality` runs
    // INVERTED on radeonsi (higher = slower), so these raise it. async_depth
    // goes to 1 on the truly deliberate presets — still parallel, never the
    // throughput sink 64 measured as.
    if (p == "slow") { if (h264) sp.quality = 2; return sp; }
    if (p == "slower" || p == "veryslow" || p == "placebo") {
        if (h264) sp.quality = (p == "placebo") ? 6 : 4;
        return sp;
    }
    return sp;  // unknown spelling: tuned default
}

void apply_vaapi_rate_control(AVCodecContext* vctx, const VaapiRateControl& rc) {
    if (!vctx) return;

    // vctx->codec is only populated by avcodec_open2(); this helper runs before
    // open, so name the encoder from the codec_id where possible.
    const AVCodecDescriptor* desc = avcodec_descriptor_get(vctx->codec_id);
    const char* codec_name = desc ? desc->name : "vaapi";
    if (!rc.rc_mode.empty()) {
        if (av_opt_set(vctx->priv_data, "rc_mode", rc.rc_mode.c_str(), 0) < 0)
            log::log_warning("[vaapi] %s: rc_mode=%s not honored (ignored)", codec_name,
                             rc.rc_mode.c_str());
    }
    if (rc.global_quality >= 0) {
        if (av_opt_set_int(vctx->priv_data, "global_quality", rc.global_quality, 0) < 0)
            log::log_warning("[vaapi] %s: global_quality not honored (ignored)", codec_name);
    }
    if (rc.qp >= 0) {
        if (av_opt_set_int(vctx->priv_data, "qp", rc.qp, 0) < 0)
            log::log_warning("[vaapi] %s: qp not honored (ignored)", codec_name);
    }
}

}  // namespace canvas::core