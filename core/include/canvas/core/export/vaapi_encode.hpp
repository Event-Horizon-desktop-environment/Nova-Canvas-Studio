#pragma once

#include <cstdint>
#include <string>

struct AVCodecContext;

namespace canvas::core {

bool is_vaapi_codec(const std::string& codec_name);

struct VaapiRateControl {
    std::string rc_mode;
    int global_quality = -1;
    int qp = -1;
};

VaapiRateControl vaapi_rate_control_from(const std::string& codec_name, int crf,
                                         const std::string& vid_rc_mode,
                                         int video_bitrate_kbps);

void apply_vaapi_rate_control(AVCodecContext* vctx, const VaapiRateControl& rc);

struct VaapiSpeed {
    int async_depth = 0;
    int quality = -1;
};

VaapiSpeed vaapi_speed_for(const std::string& codec_name, const std::string& preset);

}
