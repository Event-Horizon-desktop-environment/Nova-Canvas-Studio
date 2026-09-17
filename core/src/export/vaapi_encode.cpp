#include "canvas/core/export/vaapi_encode.hpp"

#include "canvas/core/util/log.hpp"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/opt.h>
}

#include <string>

namespace canvas::core {

namespace {

bool is_av1(const std::string& codec_name) {
    return codec_name.find("av1") != std::string::npos;
}

bool is_hevc(const std::string& codec_name) {
    return codec_name.find("hevc") != std::string::npos ||
           codec_name.find("h265") != std::string::npos;
}

}

bool is_vaapi_codec(const std::string& codec_name) {
    return codec_name.find("vaapi") != std::string::npos;
}

VaapiRateControl vaapi_rate_control_from(const std::string& codec_name, int crf,
                                         const std::string& vid_rc_mode,
                                         int video_bitrate_kbps) {
    VaapiRateControl rc;
    if (!is_vaapi_codec(codec_name)) return rc;

    if (crf >= 0) {
        const int scale = is_av1(codec_name) ? 5 : 1;
        if (vid_rc_mode == "constqp") {
            rc.rc_mode = "CQP";
            rc.qp = crf * scale;
            rc.global_quality = crf * scale;
        } else if (vid_rc_mode == "vbr" || vid_rc_mode == "vbr_target") {
            if (video_bitrate_kbps > 0 && (is_hevc(codec_name) || is_av1(codec_name))) {
                rc.rc_mode = "QVBR";
            } else {
                rc.rc_mode = "CQP";
                rc.qp = crf * scale;
            }
            rc.global_quality = crf * scale;
        } else {
            rc.rc_mode = "CQP";
            rc.qp = crf * scale;
            rc.global_quality = crf * scale;
        }
        return rc;
    }

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

    if (p == "ultrafast" || p == "superfast" || p == "veryfast" ||
        p == "fast" || p == "faster" || p.empty()) {
        return sp;
    }
    if (p == "medium") {
        if (h264) sp.quality = -1;
        return sp;
    }
    if (p == "slow") { if (h264) sp.quality = 2; return sp; }
    if (p == "slower" || p == "veryslow" || p == "placebo") {
        if (h264) sp.quality = (p == "placebo") ? 6 : 4;
        return sp;
    }
    return sp;
}

void apply_vaapi_rate_control(AVCodecContext* vctx, const VaapiRateControl& rc) {
    if (!vctx) return;

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

}
