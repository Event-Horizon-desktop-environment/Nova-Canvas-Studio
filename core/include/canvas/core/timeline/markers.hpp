#pragma once

#include "canvas/core/timeline/model.hpp"

#include <string>
#include <vector>

namespace canvas::core::markers {

struct Chapter {
    double seconds = 0.0;
    std::string label;
};

[[nodiscard]] std::vector<Chapter> chapters_from(const Sequence& seq);

}
