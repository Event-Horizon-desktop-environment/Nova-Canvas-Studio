#pragma once

#include "canvas/core/export/deliver_preset.hpp"
#include "canvas/core/timeline/model.hpp"

#include <map>
#include <string>
#include <vector>

namespace canvas::core {

struct MediaEntry {
    MediaId id = -1;
    std::string path;
    double fps = 0.0;
    int width = 0;
    int height = 0;
    int64_t total_frames = -1;
    std::string bin;
    bool has_audio = false;
};

struct Project {
    std::string name = "Untitled Project";
    std::string media_root;
    Sequence sequence;
    std::vector<MediaEntry> media;
    std::vector<std::string> bins;

    DeliverSettings deliver_settings;
    std::vector<RenderJobSnapshot> render_jobs;

    [[nodiscard]] const MediaEntry* media_by_id(MediaId id) const noexcept;
};

bool save_project(const Project& project, const std::string& path, std::string* error = nullptr);
bool load_project(Project& out, const std::string& path, std::string* error = nullptr);

}
