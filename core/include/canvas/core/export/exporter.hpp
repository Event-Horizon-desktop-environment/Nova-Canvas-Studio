#pragma once

#include "canvas/core/media/frame.hpp"

#include <functional>
#include <string>
#include <vector>

namespace canvas::core {

struct Project;

struct CodecInfo {
    std::string name;
    std::string long_name;
    int media_type = 0;
    bool hw = false;
    std::string hw_device;
};

struct ContainerInfo {
    std::string name;
    std::string long_name;
    std::string extensions;
    std::vector<std::string> default_video;
    std::vector<std::string> default_audio;
};

struct ExportControl {
    std::function<bool()> should_cancel = [] { return false; };
    std::function<void(double progress, const std::string& phase)> on_progress =
        [](double, const std::string&) {};
    std::function<void(VideoFramePtr)> on_frame = [](VideoFramePtr) {};
};

struct ExportChapter {
    double start_seconds = 0.0;
    double end_seconds = 0.0;
    std::string title;
};

struct ExportSettings {
    std::string output_path;
    std::string format;
    std::string video_codec;
    std::string audio_codec;
    int width = 0;
    int height = 0;
    double fps = 30.0;
    int64_t duration_frames = 0;
    int video_bitrate_kbps = 8000;
    int video_max_bitrate_kbps = 0;
    int audio_bitrate_kbps = 192;
    int audio_sample_rate = 48000;
    int audio_channels = 2;
    int crf = -1;
    std::string vid_rc_mode = "auto";
    std::string preset = "medium";
    std::string extra = "";
    int threads = 0;
    bool remove_audio = false;
    std::vector<ExportChapter> chapters;
};

std::vector<std::string> available_hw_devices();

std::vector<CodecInfo> list_video_codecs(const std::string& hw_device = "");
std::vector<CodecInfo> list_audio_codecs();

std::vector<ContainerInfo> list_containers();

bool export_project(const Project& project, const ExportSettings& settings,
                    ExportControl* control, std::string* error = nullptr);

}
