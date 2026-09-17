#pragma once

#include "canvas/core/project/project.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace canvas::core::edl {

[[nodiscard]] std::string timecode(int64_t frame, int fps);

[[nodiscard]] std::string write_cmx3600(const Sequence& seq, const std::string& title,
                                        const std::vector<MediaEntry>& media);

}
