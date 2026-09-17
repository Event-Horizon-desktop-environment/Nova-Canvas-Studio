#pragma once

#include <string>
#include <vector>

namespace canvas::gui {
namespace deliver_model {

const std::vector<std::string>& preset_names();

std::vector<std::string> encoder_backends();

struct EncoderBackendEntry {
    std::string label;
    std::string key;
};

std::vector<EncoderBackendEntry> available_encoder_backends();

std::vector<std::string> video_codecs_for_format(const std::string& format);

std::vector<std::string> audio_codecs_for_format(const std::string& format);

struct BitrateVisibility {
    bool show_bitrate = false;
    bool show_max = false;
    const char* bitrate_label = "Bit Rate";
};
BitrateVisibility bitrate_visibility(int rate_control_index);

}
}
