#pragma once

#include "canvas/core/media/transcript.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace canvas::core::captions {

struct Caption {
    int64_t start_ms = 0;
    int64_t end_ms = 0;
    std::string text;
};

struct Options {
    int max_chars_per_line = 42;
    int max_lines = 1;
    int64_t gap_frames = 0;
};

struct Preset {
    const char* name = nullptr;
    Options options;
};

[[nodiscard]] const std::vector<Preset>& presets() noexcept;

[[nodiscard]] const Options& preset_options(std::string_view name) noexcept;

[[nodiscard]] Options sanitize(const Options& opts) noexcept;

[[nodiscard]] std::vector<Caption> shape_captions(
    const std::vector<transcript::Segment>& segments, const Options& opts,
    double sequence_fps);

[[nodiscard]] std::string wrap_line(std::string_view text, int max_chars);

}
