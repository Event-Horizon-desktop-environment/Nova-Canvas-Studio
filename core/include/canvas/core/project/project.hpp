#pragma once

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
    std::string bin;  // name of the owning bin; empty = Master bin
};

struct Project {
    std::string name = "Untitled Project";
    Sequence sequence;
    std::vector<MediaEntry> media;
    std::vector<std::string> bins;  // user-created bin names (Master is implicit, not stored)

    [[nodiscard]] const MediaEntry* media_by_id(MediaId id) const noexcept;
};

bool save_project(const Project& project, const std::string& path, std::string* error = nullptr);
bool load_project(Project& out, const std::string& path, std::string* error = nullptr);

}
