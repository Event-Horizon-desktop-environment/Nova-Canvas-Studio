#pragma once

#include <cstdint>
#include <memory>

#include "canvas/core/project/project.hpp"

namespace canvas::gui::source_preview {

[[nodiscard]] std::shared_ptr<const canvas::core::Project> build_source_project(
    const canvas::core::MediaEntry& media, double fallback_fps);

[[nodiscard]] int64_t fraction_to_source_frame(double fraction, int64_t total_frames);
[[nodiscard]] double frame_to_fraction(int64_t frame, int64_t total_frames);

}
