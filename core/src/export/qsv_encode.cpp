#include "canvas/core/export/qsv_encode.hpp"

#include "canvas/core/util/log.hpp"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/opt.h>
}

#include <string>

namespace canvas::core {

namespace {

// AV1 VAAPI consumes a 0-255 quality scale, while H.264/HEVC qsv use the
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

bool is_qsv_codec(const std::string& codec_name) {
    return codec_name.find("qsv") != std::string::npos;
}

QsvRateControl qsv_rate_control_from(const std::string& codec_name, int crf,
                                         const std::string& vid_rc_mode,
                                         int /*video_bitrate_kbps*/) {
    QsvRateControl rc;
    if (!is_qsv_codec(codec_name)) return rc;

    // Quality-driven (crf >= 0): the exporter keeps crf and bit_rate mutually
    // exclusive, so a usable bitrate can't coexist here  these are the modes
    // that reach the encoder with bit_rate unset.
    if (crf >= 0) {
        const int scale = is_av1(codec_name) ? 5 : 1;
        if (vid_rc_mode == "constqp") {
            rc.rc_mode = "CQP";          // strict constant-QP (the "qp" knob)
            rc.qp = crf * scale;
            rc.global_quality = crf * scale;
        } else if (vid_rc_mode == "vbr" || vid_rc_mode == "vbr_target") {
            // QVBR is the closest thing VAAPI has to x264 CRF: it treats the
            // quality value as a perceptual target and lets the rate float.
            // h264_qsv's rc_mode list doesn't carry QVBR  ICQ is its CRF
            // analog  so route H.264 to ICQ and H.265/AV1 to QVBR.
            rc.rc_mode = is_hevc(codec_name) || is_av1(codec_name) ? "QVBR" : "ICQ";
            rc.global_quality = crf * scale;
        } else {                         // "auto" (or unspecified)
            rc.rc_mode = "ICQ";          // constant quality, universally supported
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

void apply_qsv_rate_control(AVCodecContext* vctx, const QsvRateControl& rc) {
    if (!vctx) return;

    // vctx->codec is only populated by avcodec_open2(); this helper runs before
    // open, so name the encoder from the codec_id where possible.
    const AVCodecDescriptor* desc = avcodec_descriptor_get(vctx->codec_id);
    const char* codec_name = desc ? desc->name : "qsv";
    if (!rc.rc_mode.empty()) {
        if (av_opt_set(vctx->priv_data, "rc_mode", rc.rc_mode.c_str(), 0) < 0)
            log::log_warning("[qsv] %s: rc_mode=%s not honored (ignored)", codec_name,
                             rc.rc_mode.c_str());
    }
    if (rc.global_quality >= 0) {
        if (av_opt_set_int(vctx->priv_data, "global_quality", rc.global_quality, 0) < 0)
            log::log_warning("[qsv] %s: global_quality not honored (ignored)", codec_name);
    }
    if (rc.qp >= 0) {
        if (av_opt_set_int(vctx->priv_data, "qp", rc.qp, 0) < 0)
            log::log_warning("[qsv] %s: qp not honored (ignored)", codec_name);
    }
}

}  // namespace canvas::core