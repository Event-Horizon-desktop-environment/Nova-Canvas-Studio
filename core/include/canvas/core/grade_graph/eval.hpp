#pragma once

// Headless CPU evaluator for the node tree (grade_graph). Qt-free. This is the
// single reference implementation that preview (TimelineDecoder path), export
// (RenderSession), and scopes will share in Phase 3+; any GPU fast path built
// later must match it (same guard culture as cuda_available()).
//
// Evaluation rules (node-graph-system-implementation-spec.md §3):
//   1. Topological sort of the DAG across BOTH pipes (rgb + key can have
//      different topologies). Cycles are rejected at connect time by the
//      model, but the evaluator re-verifies defensively.
//   2. The shared upstream of Parallel/Layer branches is evaluated once and
//      reused (outputs are cached for the duration of one evaluate() call).
//   3. Exactly-one-output rule: if no node's rgb output reaches the terminal,
//      the whole tree is inactive and the clip passes through (no error).
//   4. Correctors blend their correction by their key:
//        node_output = mix(rgb_input, corrected, key_input) * opacity
//   5. A node with no key input uses a uniform key of 1.0.

#include "canvas/core/grade_graph/graph.hpp"

#include <memory>
#include <string>
#include <vector>

namespace canvas::core::grade_graph {

// Float RGBA image (linear-ish working space, no quantization between ops).
// Alpha rides along for layer-mixer convenience but the node math operates on
// RGB; the key pipe is a separate single-channel image below.
struct FrameF {
    int w = 0;
    int h = 0;
    std::vector<float> rgba;  // w*h*4, row-major R,G,B,A

    static FrameF filled(int w, int h, float r, float g, float b, float a = 1.0f);
    [[nodiscard]] float* at(int x, int y);
    [[nodiscard]] const float* at(int x, int y) const;
};

// Single-channel float image: key mattes and splitter channel outputs share
// this value type but are distinct graph wire types (see PipeType).
struct GrayF {
    int w = 0;
    int h = 0;
    std::vector<float> v;

    static GrayF filled(int w, int h, float value);
};

struct EvalResult {
    // nullptr => the node tree is inactive and the clip passes through.
    std::shared_ptr<const FrameF> frame;
    // Non-empty => invalid graph (e.g. a cycle slipped past connect-time
    // rejection); frame is null.
    std::string error;
};

// Evaluates the node tree over `source`. Result ownership mirrors the caller's
// expectation: frames are shared_const, never mutated after evaluation.
[[nodiscard]] EvalResult evaluate_graph(const GradeGraph& g, const FrameF& source);

// Over-with-blend compositing law (see composite.hpp): `layer` is composited
// over `acc` with the given blend family, Porter-Duff alpha recomputed from
// each pixel's alpha (source = layer[3], backdrop = acc[3]). Opaque inputs
// reproduce the classic blend-family "replace" behavior. The Layer Mixer uses
// the per-node composite law directly (folding key*opacity into coverage);
// this convenience stays available for exact-Over callers.
void blend_into(const float* acc, const float* layer, float* out, std::size_t n,
                BlendMode blend);

}  // namespace canvas::core::grade_graph