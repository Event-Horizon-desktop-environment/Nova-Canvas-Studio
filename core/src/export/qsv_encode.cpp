#include "canvas/core/export/qsv_encode.hpp"

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

bool is_qsv_codec(const std::string& codec_name) {
    return codec_name.find("qsv") != std::string::npos;
}

QsvRateControl qsv_rate_control_from(const std::string& codec_name, int crf,
                                         const std::string& vid_rc_mode,
                                         int) {
    QsvRateControl rc;
    if (!is_qsv_codec(codec_name)) return rc;

    if (crf >= 0) {
        const int scale = is_av1(codec_name) ? 5 : 1;
        if (vid_rc_mode == "constqp") {
            rc.rc_mode = "CQP";
            rc.qp = crf * scale;
            rc.global_quality = crf * scale;
        } else if (vid_rc_mode == "vbr" || vid_rc_mode == "vbr_target") {
            rc.rc_mode = is_hevc(codec_name) || is_av1(codec_name) ? "QVBR" : "ICQ";
            rc.global_quality = crf * scale;
        } else {
            rc.rc_mode = "ICQ";
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

void apply_qsv_rate_control(AVCodecContext* vctx, const QsvRateControl& rc) {
    if (!vctx) return;

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

}
