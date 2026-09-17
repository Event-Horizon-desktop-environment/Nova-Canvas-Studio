#pragma once

#include "canvas/core/timeline/captions.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace canvas::core::caption_burn {

struct Window {
    int64_t start = 0;
    int64_t end = 0;
    std::string text;
};

[[nodiscard]] std::vector<Window> windows_from(const std::vector<captions::Caption>& caps,
                                               double fps);

[[nodiscard]] int active_at(const std::vector<Window>& windows, int64_t tl_frame) noexcept;

[[nodiscard]] std::string text_at(const std::vector<Window>& windows, int64_t tl_frame);

}
