#pragma once

#include <cstdint>
#include <string>

struct AVCodecContext;

namespace canvas::core {

bool is_qsv_codec(const std::string& codec_name);

struct QsvRateControl {
    std::string rc_mode;
    int global_quality = -1;
    int qp = -1;
};

QsvRateControl qsv_rate_control_from(const std::string& codec_name, int crf,
                                         const std::string& vid_rc_mode,
                                         int video_bitrate_kbps);

void apply_qsv_rate_control(AVCodecContext* vctx, const QsvRateControl& rc);

}
