#pragma once

// Headless node-graph MODEL for the Color page's node tree. Qt-free by design
// (same invariant as core/ and the extracted GUI headless modules) so the
// graph logic runs in plain unit tests; the Qt canvas in Phase 6 becomes a
// thin view over this model.
//
// Dual-pipe signal model (node-graph-system-implementation-spec.md):
//   - RGB pipe (green): the image data, left→right.
//   - Key pipe  (blue): single-channel 0..1 mattes that modulate how strongly
//     each node's OWN correction is blended back. The key pipe never appears
//     in an RGB output; it only gates the blend of that node's correction.
//   - Channel wire (splitter/combiner): single-channel grayscale data, typed
//     separately so the UI cannot wire a channel into a key port (or vice
//     versa) even though both are single-float per pixel.
//
// A node with no key input connected defaults to a uniform key of 1.0
// ("apply everywhere").
//
// The graph is a plain data structure (nodes + typed edges + per-node params),
// serialization-friendly for the project file (Phase 3). It does not evaluate:
// the evaluator lives in eval.hpp/.cpp (namespace-mates in this folder), per
// splitplan "one class, one file, one job".

#include "canvas/core/colorsci/cdl.hpp"
#include "canvas/core/colorsci/curves.hpp"
#include "canvas/core/colorsci/wheels.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace canvas::core::grade_graph {

// Wire family — deliberately distinct enum values so the UI cannot cross-wire
// a key output into an RGB input (or a splitter channel into a key port).
enum class PipeType { kRgb, kKey, kChannel };

enum class NodeKind {
    kCorrector,      // serial corrector: rgb in/out + key in/out (key passes through)
    kParallel,       // branch node: rgb in (shared source), own key, rgb out
    kLayer,          // branch node feeding a Layer Mixer; own key, rgb out
    kOutside,        // corrector whose key input is auto-wired to 1 - partner.key_out
    kKeyMixer,       // pure key math: key in(s), key out, NO rgb ports
    kSplitter,       // rgb in -> R,G,B channel outs
    kCombiner,       // R,G,B channel ins -> rgb out
    kParallelMixer,  // additive combiner: out = A + B - shared_base
    kLayerMixer,     // blend compositor: base + layer branches (explicit stack order)
    kOutput,         // graph terminal; exactly one rgb in
};

// The per-node op slot (see op.hpp/.cpp for the registry that owns apply /
// identity / name dispatch). What a Corrector/Parallel/Layer/Outside node's
// own formula computes; identity is the safe default so an empty node is a
// no-op. Phase 6b widened this from CorrectMode to OpKind so Phase 7 effect
// ops land under the same slot; `CorrectMode` is kept as a deprecated alias
// for the existing callers/project files.
enum class OpKind { kIdentity, kLgg, kCdl, kCurves };
using CorrectMode = OpKind;  // deprecated spelling, kept for source compat

enum class KeyMixMode { kAdd, kSubtract, kIntersect, kInvert };

// Porter–Duff compositing operator a Layer Mixer applies when folding a layer
// in (Phase 6a). kOver + kNormal reproduces the throwback straight composite
// for opaque sources; the rest are the classic set (Fusion Merge naming). The
// law equations live in composite.hpp/.cpp — the model only stores the choice.
// Semantics at opaque coverage: Over/In/Atop/Disjoint reproduce the source (or
// its blend-family result), Out/XOr/Stencil cut to transparent, Mask keeps the
// backdrop under the source alpha, Stencil cuts a hole with the inverse.
enum class CompositeOp {
    kOver,
    kIn,
    kOut,
    kAtop,
    kXor,
    kDisjoint,
    kMask,
    kStencil,
};

enum class BlendMode {
    kNormal,      // straight alpha composite: below*(1-k) + layer*k
    kScreen,      // 1 - (1-a)(1-b)
    kMultiply,    // a*b
    kOverlay,     // a<=0.5 ? 2ab : 1 - 2(1-a)(1-b)
    kSoftLight,   // Photoshop-family soft light
    kAdd,         // a + b
    kSubtract,    // a - b
    kDifference,  // |a - b|
};

struct Node {
    int id = 0;
    NodeKind kind = NodeKind::kCorrector;
    std::string label;
    bool bypass = false;      // skip this node's correction (rgb passes through)
    float opacity = 1.0f;     // extra gate on the node's own correction blend

    // kOutside: upstream partner whose *key out* we take the complement of.
    int partner = -1;
    // kParallel/parallel-mixer wiring: the node whose rgb output feeds the
    // shared base both branches start from.
    int shared_source = -1;

    // Op slot (OpKind) + params (Phase 1 laws; Phase 3 adds the full tone set).
    // The field keeps its Phase 3 name for JSON/project-file compatibility.
    OpKind correct_mode = OpKind::kIdentity;
    std::optional<colorsci::LGG> lgg;
    std::optional<colorsci::Cdl> cdl;
    std::optional<colorsci::Offset> offset;  // Primaries Offset wheel, applied before LGG
    std::optional<colorsci::CurveParams> curves;

    KeyMixMode key_mode = KeyMixMode::kAdd;
    BlendMode blend = BlendMode::kNormal;
    // Layer-fold law (Layer Mixer reads these per layer node): the composite
    // operator and Fusion-style additive/subtractive knob (0.0 = subtractive,
    // premultiplied edges; 1.0 = additive, backdrop unattenuated). Ignored by
    // serial/parallel/corrector nodes.
    CompositeOp composite_op = CompositeOp::kOver;
    float additive = 0.0f;
};

struct PipeId {
    int node = -1;
    PipeType type = PipeType::kRgb;
    int port = 0;  // splitter channel outs / combiner channel ins use 0..2; rgb/key use 0
};

struct Edge {
    PipeId from;
    PipeId to;
};

class GradeGraph {
public:
    // Returns the new node's stable id; ids are sequential from 0, so node(id)
    // is O(1). Caller configures kind/params via the returned Node&.
    int add_node(NodeKind kind);
    [[nodiscard]] Node& node(int id);
    [[nodiscard]] const Node& node(int id) const;

    // Wires. Returns the new edge index, or -1 when the wire would create a
    // cycle (verified at connect time, per the spec — never at render time).
    // The generic add_edge accepts any typed pipe/port; the typed wrappers
    // below delegate to it. remove_edge deletes the exact wire (used by the
    // Phase 6 graph-edit laws to rewire branch insertion / layer reorder).
    [[nodiscard]] int add_edge(PipeId from, PipeId to);
    int add_rgb_edge(int from, int to);
    int add_key_edge(int from, int to);
    int add_channel_edge(int from, int port, int to, int port2);
    [[nodiscard]] bool remove_edge(PipeId from, PipeId to);
    [[nodiscard]] bool would_create_cycle(PipeId from, PipeId to) const;

    // The terminal node (kind kOutput), or -1 when the tree is inactive.
    [[nodiscard]] int terminal() const;
    // RGB/key/channel inputs of a node, per port. An unconnected RGB input
    // reads the clip source; an unconnected key input defaults to uniform 1.0.
    [[nodiscard]] std::optional<int> rgb_source_of(int node) const;
    // (port, source node) per key-input edge (port 0 = the primary key input).
    [[nodiscard]] std::vector<std::pair<int, int>> key_sources_of(int node) const;
    // (port, source node) per channel-input edge (splitter outs / combiner ins).
    [[nodiscard]] std::vector<std::pair<int, int>> channel_sources_of(int node) const;

    [[nodiscard]] std::size_t num_nodes() const;
    [[nodiscard]] const std::vector<Edge>& edges() const;
    void clear();

    // Transient change-tracking token (NOT serialized): bumped by the Color
    // page on every grade write so the always-on log chain (commit → bake →
    // upload → draw) reads as one sequence. Lives only in memory; the JSON
    // round-trip ignores it.
    std::uint64_t change_seq = 0;

private:
    std::vector<Node> nodes_;
    std::vector<Edge> edges_;
};

}  // namespace canvas::core::grade_graph