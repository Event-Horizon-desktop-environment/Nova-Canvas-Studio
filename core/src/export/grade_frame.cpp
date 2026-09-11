#include "canvas/core/export/grade_frame.hpp"

#include "canvas/core/grade_graph/eval.hpp"

#include <algorithm>
#include <cmath>

namespace canvas::core {

namespace {

constexpr float kByteToUnit = 1.0f / 255.0f;

// Clamps a unit-space float back to a byte. NaN/denormal-safe for the same
// reason as the colorsci laws: the evaluator never emits them, but clamping
// keeps a corrupt graph from producing garbage bytes.
std::uint8_t clamp_unit(float v) {
    v = std::max(0.0f, std::min(1.0f, v));
    return static_cast<std::uint8_t>(std::lround(v * 255.0f));
}

}  // namespace

VideoFramePtr apply_grade_to_frame(const VideoFrame& src,
                                   const grade_graph::GradeGraph& grade) {
    if (src.width <= 0 || src.height <= 0 || src.rgba.empty()) return nullptr;

    grade_graph::FrameF in;
    in.w = src.width;
    in.h = src.height;
    in.rgba.resize(src.rgba.size());
    for (std::size_t i = 0; i < src.rgba.size(); ++i)
        in.rgba[i] = static_cast<float>(src.rgba[i]) * kByteToUnit;

    const grade_graph::EvalResult res = grade_graph::evaluate_graph(grade, in);
    if (!res.frame) return nullptr;  // no terminal => passthrough
    if (!res.error.empty()) return nullptr;

    const grade_graph::FrameF& out = *res.frame;
    if (out.w != in.w || out.h != in.h || out.rgba.size() != in.rgba.size()) return nullptr;

    auto graded = std::make_shared<VideoFrame>();
    graded->pts_ticks = src.pts_ticks;
    graded->pts_seconds = src.pts_seconds;
    graded->frame_number = src.frame_number;
    graded->width = src.width;
    graded->height = src.height;
    graded->stride = src.stride;
    graded->rgba.resize(out.rgba.size());
    for (std::size_t i = 0; i < out.rgba.size(); ++i)
        graded->rgba[i] = clamp_unit(out.rgba[i]);
    return graded;
}

}  // namespace canvas::core