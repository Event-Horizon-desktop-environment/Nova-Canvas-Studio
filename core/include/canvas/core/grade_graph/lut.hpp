#pragma once

// Resolve-style grade flattening: bakes a pointwise node-grade tree into a 3D
// RGB->RGB LUT that can be applied on the GPU (viewer fragment shader) or on
// the CPU (export / scopes) with the SAME law. Qt-free, unit-testable.
//
// WHY a LUT: the whole grade graph today is a per-pixel function of RGB only
// (correctors + key math over color-derived mattes; no spatial power-window /
// qualifier key producers exist yet). Sampling `evaluate_graph` on a grid and
// trilinear-interpolating reproduces it on any device. Anything spatial that
// appears later must be rejected here (bake returns null) and routed back to
// the per-pixel CPU evaluator.
//
// SAMPLING CONVENTION (kept identical on GPU and CPU so preview == export):
//   * Gridpoints at input positions p_i = i/(N-1), i in [0, N-1], stored at
//     texel/grid index i.
//   * Sequential-access coordinate u = x * (N-1) in grid units; fractional
//     parts pick the pairwise interpolation weight between neighbors floor(u)
//     and floor(u)+1 (clamped). This mirrors GL_TEXTURE_LINEAR on a 3D texture
//     whose coord is set to `x*(N-1)/N + 0.5/N` (texel-center pass-through).
//
// bake_grade_lut evaluates the whole N^3 grid as ONE image (a single
// evaluate_graph call over a FrameF whose pixel (x, y) carries grid coordinate
// ri=x, gi=y/N, bi=y%N), so baking a 33^3 grid costs about one full-frame
// pass — ~57k single-pixel evaluations instead of ~36k separate calls. It is
// meant to be cached per (clip, grade) and rebuilt only on grade changes.

#include "canvas/core/grade_graph/graph.hpp"
#include "canvas/core/media/frame.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace canvas::core::grade_graph {

struct GradeLut3D {
    // Cells per axis. data has size*size*size * 3 floats (R,G,B per cell).
    int size = 0;
    std::vector<float> data;
    // Copied from the source graph's change_seq at bake so the [grade] log
    // chain (commit → bake → upload → draw) is correlateable by sequence.
    std::uint64_t change_seq = 0;

    [[nodiscard]] bool valid() const noexcept {
        return size > 1 && data.size() == static_cast<std::size_t>(size) *
                                                   static_cast<std::size_t>(size) *
                                                   static_cast<std::size_t>(size) * 3u;
    }
};

// Compact, log-friendly summary of a baked LUT used to pin down grade-color
// bugs (purple / channel-swapped output) at the bake vs. upload boundary.
//   hash     — FNV-1a over the raw RGB float bits; bake and upload must agree.
//   mid      — output value at the grid center (mid-gray input, ~(0.5,0.5,0.5)).
//   black/white — output at grid corners (0,0,0) and (1,1,1).
//   max_dev  — largest per-channel |output - gridpoint| over the whole grid;
//              ~0 means near-identity, anything large means real grading is on.
// A channel swap shows as mid/black/white carrying the wrong per-channel order.
struct GradeLutDigest {
    std::uint64_t hash = 0;
    float mid[3] = {0.0f, 0.0f, 0.0f};
    float black[3] = {0.0f, 0.0f, 0.0f};
    float white[3] = {0.0f, 0.0f, 0.0f};
    float max_dev = 0.0f;
    // Output at a skin-tone probe input (~0.54, 0.36, 0.31). A channel swap
    // or a strong complementary cast shows up here (e.g. a blue-heavy skin
    // sample) far earlier than anywhere in the per-frame digests, so a
    // "face turned purple" repro is visible on the very first bake.
    std::array<float, 3> skin = {0.0f, 0.0f, 0.0f};
};

[[nodiscard]] GradeLutDigest grade_lut_digest(const GradeLut3D& lut) noexcept;

using GradeLutPtr = std::shared_ptr<const GradeLut3D>;

// Samples the graph's per-pixel function on an N^3 grid. Returns nullptr when
// the graph has no terminal (inactive / passthrough), or when it contains
// anything non-pointwise a 3D LUT cannot represent.
[[nodiscard]] GradeLutPtr bake_grade_lut(const GradeGraph& g, int size = 33);

// Trilinear CPU application using the grid convention above. Returns a NEW
// graded copy of src, or nullptr when `lut` is invalid/empty so callers fall
// back to the original frame (same passthrough contract as
// apply_grade_to_frame). Never mutates src.
[[nodiscard]] VideoFramePtr apply_grade_lut(const VideoFrame& src,
                                            const GradeLut3D& lut);

}  // namespace canvas::core::grade_graph