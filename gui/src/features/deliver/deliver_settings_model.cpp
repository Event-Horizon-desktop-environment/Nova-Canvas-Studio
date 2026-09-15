#include "features/deliver/deliver_settings_model.hpp"

#include "canvas/core/export/deliver_preset.hpp"
#include "canvas/core/media/gpu_select.hpp"
#include "canvas/core/media/hw_device.hpp"

#include <algorithm>
#include <cctype>
#include <string>
#include <vector>

namespace canvas::gui {
namespace deliver_model {

namespace {

std::string lowered(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) -> char {
        return static_cast<char>(std::tolower(c));
    });
    return s;
}

bool has_vendor(const std::string& vendor) {
    for (const auto& g : canvas::core::gpu_select::detect_gpus()) {
        if (g.vendor == vendor) return true;
    }
    return false;
}

// Resolve the Preferences GPU pin (set_preferred_gpu backend + device_arg) to
// a vendor string. "" means no pin, so every detected vendor is offered.
std::string pinned_vendor() {
    const std::string& pg = canvas::core::HwDeviceManager::preferred_gpu_backend();
    if (pg.empty()) return {};
    const std::string& pa = canvas::core::HwDeviceManager::preferred_device_arg();
    for (const auto& g : canvas::core::gpu_select::detect_gpus()) {
        // A backend-only pin (empty device_arg) matches any GPU of that backend;
        // a device pin must name the exact render node / CUDA ordinal.
        if (g.backend == pg && (pa.empty() || g.device_arg == pa)) return g.vendor;
    }
    // The pinned GPU is not in the current detection (e.g. a render node that
    // disappeared since the preference was set): don't guess a vendor, show
    // every detected encoder — the exporter's strict-pin law still drops any
    // backend that doesn't match the pin at encode time.
    return {};
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

std::vector<EncoderBackendEntry> available_encoder_backends() {
    // Auto + CPU are always possible; the vendor entries appear only when a
    // GPU of that vendor is present (the Settings dialog's hardware list and
    // the exporter's encoders draw from the same detection). When Preferences
    // has a specific GPU pinned, only that GPU's vendor appears here — an
    // AMD-pinned user sees AMD VAAPI and no NVIDIA NVENC; Auto (no pin) shows
    // every detected vendor.
    std::vector<EncoderBackendEntry> out;
    out.push_back({"Auto", "Auto"});
    out.push_back({"CPU", "CPU"});
    const std::string pv = pinned_vendor();
    const bool restricted = !pv.empty();
    if (has_vendor("NVIDIA") && (!restricted || pv == "NVIDIA"))
        out.push_back({"NVIDIA NVENC", "NVIDIA"});
    if (has_vendor("AMD") && (!restricted || pv == "AMD"))
        out.push_back({"AMD VAAPI", "AMD"});
    if (has_vendor("Intel") && (!restricted || pv == "Intel"))
        out.push_back({"Intel QSV", "Intel"});
    return out;
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