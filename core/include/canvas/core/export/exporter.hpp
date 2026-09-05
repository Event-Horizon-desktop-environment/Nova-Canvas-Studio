#pragma once

#include "canvas/core/project/project.hpp"

#include <functional>
#include <string>
#include <vector>

namespace canvas::core {

// A codec/encoder known to the system, for the "every format" codec list.
struct CodecInfo {
    std::string name;    // FFmpeg encoder name (e.g. "libx264", "h264_nvenc")
    std::string long_name;
    int media_type = 0;  // AVMEDIA_TYPE_VIDEO / AVMEDIA_TYPE_AUDIO
    bool hw = false;     // hardware-accelerated encoder
    std::string hw_device;  // e.g. "cuda", "vaapi", "qsv", "amf" ("" = CPU)
};

// A muxer/container available on the system.
struct ContainerInfo {
    std::string name;      // e.g. "mp4"
    std::string long_name;
    std::string extensions;  // dot-separated list, e.g. "mp4,m4v,mov"
    std::vector<std::string> default_video;
    std::vector<std::string> default_audio;
};

// Polled cancellation + progress for the render/encode loop.
struct ExportControl {
    std::function<bool()> should_cancel = [] { return false; };
    std::function<void(double progress, const std::string& phase)> on_progress =
        [](double, const std::string&) {};
};

// Everything needed to produce an output file.
struct ExportSettings {
    std::string output_path;
    std::string format;       // muxer name, e.g. "mp4"
    std::string video_codec;  // FFmpeg encoder name, e.g. "libx264" / "h264_nvenc"
    std::string audio_codec;  // e.g. "aac" / "libmp3lame" / "" (no audio)
    int width = 0;
    int height = 0;
    double fps = 30.0;
    int64_t duration_frames = 0;
    int video_bitrate_kbps = 8000;   // 0 = use quality (crf)
    int video_max_bitrate_kbps = 0;  // 0 = no cap; else VBV/maxrate ceiling (kbps)
    int audio_bitrate_kbps = 192;
    int audio_sample_rate = 48000;
    int audio_channels = 2;
    int crf = -1;                   // -1 = use bitrate; else constant quality
    // Rate-control intent carried from DeliverSettings so the encoder can be
    // told to honor the target/ceiling explicitly. One of:
    //   "auto"      - let the encoder pick (no explicit rc option)
    //   "constqp"   - constant quality (crf / cq), no bitrate
    //   "vbr"       - quality-ish VBR toward target, capped by max
    //   "vbr_target"- 1-pass VBR targeting video_bitrate_kbps
    //   "cbr"       - true constant bitrate (bit_rate == maxrate, small bufsize)
    std::string vid_rc_mode = "auto";
    std::string preset = "medium";  // x264 preset or nvenc preset
    std::string extra = "";         // extra libavcodec options, "k=v\nk=v"
    bool remove_audio = false;      // if a codec is selected this is ignored
};

// Returns available (detected) hardware-acceleration device names for encoding
// (e.g. "cuda", "vaapi", "qsv", "amf"). Probes the current machine.
std::vector<std::string> available_hw_devices();

// Returns all encoders FFmpeg was built with that can encode video/audio,
// optionally filtered by a hardware device ("" = software/cpu only).
std::vector<CodecInfo> list_video_codecs(const std::string& hw_device = "");
std::vector<CodecInfo> list_audio_codecs();

// Returns all muxers FFmpeg was built with that are real file containers.
std::vector<ContainerInfo> list_containers();

// Encodes the project timeline to `settings.output_path`. Calls back with
// progress. Returns true on success; fills `error` on failure.
bool export_project(const Project& project, const ExportSettings& settings,
                    ExportControl* control, std::string* error = nullptr);

}  // namespace canvas::core
