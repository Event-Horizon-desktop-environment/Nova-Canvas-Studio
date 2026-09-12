#pragma once

// Applies a clip's node-grade tree to a decoded RGBA frame (Phase 3). Pure
// adapter: converts VideoFrame bytes <-> the evaluator's float FrameF, runs
// grade_graph::evaluate_graph, and converts back. Qt-free. The evaluator in
// eval.hpp stays image-domain-agnostic; this module is where the export path
// (and later the preview path) meets the node tree at actual pixels.

#include "canvas/core/grade_graph/graph.hpp"
#include "canvas/core/media/frame.hpp"

#include <memory>

namespace canvas::core {

// Returns a NEW graded copy of `src`, or nullptr when the graph is wired but
// has no terminal (evaluate_graph passthrough) so callers can fall back to the
// original frame. Callers gate on Clip::has_grade before calling, so the cheap
// no-grade path never lands here. Never mutates `src`.
[[nodiscard]] VideoFramePtr apply_grade_to_frame(const VideoFrame& src,
                                                 const grade_graph::GradeGraph& grade);

}  // namespace canvas::core