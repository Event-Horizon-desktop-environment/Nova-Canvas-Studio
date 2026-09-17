#pragma once

#include "canvas/core/grade_graph/graph.hpp"
#include "canvas/core/media/frame.hpp"

#include <memory>

namespace canvas::core {

[[nodiscard]] VideoFramePtr apply_grade_to_frame(const VideoFrame& src,
                                                 const grade_graph::GradeGraph& grade);

}
