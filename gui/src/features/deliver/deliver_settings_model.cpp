#include "features/deliver/deliver_settings_model.hpp"

#include "canvas/core/export/deliver_preset.hpp"

#include <algorithm>
#include <cctype>

namespace canvas::gui {
namespace deliver_model {

namespace {

std::string lowered(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) -> char {
        return static_cast<char>(std::tolower(c));
    });
    return s;
}

}  // namespace

const std::vector<std::string>& preset_names() {
    static const std::vector<std::string> kPresets = {
        "Custom Export", "YouTube 2160p", "YouTube 1440p", "YouTube 1080p",
        "Vimeo 4K",      "H.265 MKV Best", "H.264 MP4 Web",
    };
    return kPresets;
}

std::vector<std::string> encoder_backends() {
    return canvas::core::deliver_encoders();
}

std::vector<std::string> video_codecs_for_format(const std::string& format) {
    const std::string fmt = lowered(format);
    if (fmt.find("mkv") != std::string::npos)
        return {"H.264", "H.265", "AV1", "Apple ProRes", "FFV1", "JPEG 2000", "Uncompressed"};
    if (fmt.find("mp4") != std::string::npos)
        return {"H.264", "H.265", "AV1"};
    if (fmt.find("quicktime") != std::string::npos || fmt == "mov")
        return {"H.264", "H.265", "Apple ProRes", "FFV1", "Uncompressed"};
    if (fmt == "webm")
        return {"AV1"};  // WebM allows VP8/VP9/AV1; we expose AV1 here
    if (fmt.find("avi") != std::string::npos)
        return {"H.264", "H.265", "FFV1", "Uncompressed"};
    if (fmt.find("mxf") != std::string::npos || fmt.find("imf") != std::string::npos)
        return {"H.264", "H.265"};
    if (fmt.find("mpeg-2") != std::string::npos || fmt == "mpeg")
        return {"H.264"};
    // Image-sequence formats: codec is irrelevant at the muxer level.
    if (fmt.find("png") != std::string::npos || fmt.find("dpx") != std::string::npos ||
        fmt.find("exr") != std::string::npos || fmt.find("jpeg") != std::string::npos ||
        fmt.find("tiff") != std::string::npos || fmt.find("webp") != std::string::npos ||
        fmt.find("gif") != std::string::npos)
        return {"Uncompressed"};
    return {"H.264", "H.265", "AV1"};
}

std::vector<std::string> audio_codecs_for_format(const std::string& format) {
    const std::string fmt = lowered(format);
    if (fmt == "webm")
        return {"Opus", "Vorbis"};
    if (fmt.find("mpeg-2") != std::string::npos || fmt == "mpeg")
        return {"MP3"};
    if (fmt.find("avi") != std::string::npos)
        return {"PCM", "MP3"};
    if (fmt.find("mxf") != std::string::npos || fmt.find("imf") != std::string::npos)
        return {"PCM"};
    return {"AAC", "MP3", "PCM", "FLAC", "Opus", "Vorbis"};
}

BitrateVisibility bitrate_visibility(const int rate_control_index) {
    BitrateVisibility v;
    // RateControl: 0=ConstantQP, 1=VBR(Quality), 2=VBR(Target), 3=ConstantBitrate.
    v.show_bitrate = (rate_control_index == 2 || rate_control_index == 3);
    v.show_max = (rate_control_index == 2);
    v.bitrate_label = rate_control_index == 2 ? "Target (Kbps)" : "Bit Rate";
    return v;
}

}  // namespace deliver_model
}  // namespace canvas::gui