#pragma once

// Phase 6 headless graph-EDIT laws for the Color page's node canvas. Qt-free,
// same culture as the model/evaluator it sits on top of (grade_graph/):
//
//   can_wire / connect_or_replace  — connection validity (type match, port
//                                    existence, single-owner ports, cycles).
//   node_ports                     — the typed port strip each kind exposes, so
//                                    the canvas draws green-RGB squares vs
//                                    blue-key triangles / channel ports from ONE
//                                    table instead of re-deriving it locally.
//   insert_branch                  — Parallel/Layer branch creation with the
//                                    matching mixer AUTO-CREATED and wired into
//                                    the serial chain (no user wiring needed).
//   add_layer / set_layer_order    — Layer Stack edits that move only port
//                                    numbers, never a wire's attachment.
//
// The Qt NodeGraphCanvas (Phase 6) becomes a thin view over these laws; the
// phase-2 evaluator already defines what the fixed points are (an identity
// branch is a no-op, layer reorder preserves every link), and the tests here
// pin both so view and evaluator cannot drift.

#include "canvas/core/grade_graph/graph.hpp"

namespace canvas::core::grade_graph {

// Why a proposed `from -> to` wire was (or wasn't) admitted. Short reason
// strings are surfaced verbatim by the canvas's hover/refusal tooltip.
struct WireRule {
    bool ok = false;
    const char* reason = "?";
};

// Layer Mixer accepts any number of layer inputs on ports 1..N; every other
// kind has a fixed port count. node_ports uses this sentinel only for that.
inline constexpr int kUnboundedPorts = -1;

struct NodePorts {
    int rgb_in = 0;            // fixed rgb input ports (Layer Mixer: 1 base + unbounded layers)
    int rgb_out = 0;           // rgb output ports (always 0 or 1, port 0)
    int key_in = 0;            // key input ports (Key Mixer: 2 — A on 0, B on 1)
    int key_out = 0;           // key output ports (port 0)
    int channel_in = 0;        // combiner: 3 (ports 0..2)
    int channel_out = 0;       // splitter: 3 (ports 0..2)
    bool unbounded_rgb_in = false;
};

// Typed port arity per node kind — the single source of truth for both the
// canvas's port strip and can_wire's port-existence checks. Note the Outside
// node exposes NO key input: its partition key is auto-wired to
// 1 - partner.key_out by the evaluator, so a manual key wire would be inert.
[[nodiscard]] NodePorts node_ports(NodeKind kind);

// Whether appending `from -> to` is legal on the CURRENT graph: real distinct
// nodes, matching pipe type, an existing destination port on the target kind,
// a single-owner destination port (a port may carry one wire — rgb in port 0,
// key in ports, combiner/splitter channel ports, mixer A/B, Layer Mixer base
// and each layer slot), and no cycle. An occupied port answers ok=false with
// reason "occupied": the canvas re-routes through connect_or_replace.
[[nodiscard]] WireRule can_wire(const GradeGraph& g, PipeId from, PipeId to);

// Connect `from -> to`, replacing any existing single wire on the destination
// port (a wire drag onto an occupied port re-routes instead of failing).
// Returns the new edge index, or -1 when the wire is illegal for a non-
// occupied reason (or would create a cycle).
[[nodiscard]] int connect_or_replace(GradeGraph& g, PipeId from, PipeId to);

// Insert a kParallel or kLayer branch over `sink`'s current rgb input and
// auto-create + wire the matching mixer (kParallelMixer / kLayerMixer), so the
// branch reads the same source the serial chain did, the identity path keeps
// feeding the mixer, and the mixer's output replaces that source as sink's
// input:
//
//   before:  src ---------------------> sink
//   after:   src --+--> branch --+--> mixer --+--> sink   (kParallel):   A+B-base
//                  +--> (src) ---+             |            (kLayer):     base + layer
//                   (mixer's shared_source = src)          (mixer ports: 0 == base/A,
//                                                          1 == branch; edge order
//                                                          matters — see below)
//
// WIRE-ORDER CONTRACT: base/A must be wired into the mixer BEFORE the branch,
// because the evaluator reads a mixer's rgb inputs in edge order (the A slot
// is whichever edge comes first, not port order). This module emits base first;
// callers must not reorder those two add_edge calls.
//
// BOTH branch kinds require a real wired source on `sink` (an unwired base is
// refused): the evaluator treats a lone layer edge as the base — skipping its
// key gate — and a Parallel Mixer with a missing first operand yields null, so
// inserting over the chain head is not well-defined until a source-bridge
// node exists (Phase 6+ follow-up). Returns the MIXER node id, or -1 for an
// invalid kind / sink / missing source.
[[nodiscard]] int insert_branch(GradeGraph& g, NodeKind kind, int sink);

// Append `layer` as the next (topmost) layer input of a Layer Mixer, assigning
// the lowest free layer port >= 1. Returns the assigned port, or -1 when the
// wire is illegal or `layer` is already a layer of the mixer.
[[nodiscard]] int add_layer(GradeGraph& g, int mixer, int layer);

// Reorder a Layer Mixer's visible stack into `ordered` (the layer SOURCE node
// ids, bottom-to-top). Only the layer-input PORT NUMBERS move (compacted to
// 1..N); every wire stays attached to its node, so reorder preserves links by
// construction. Returns the layer count, or -1 when `mixer` is not a Layer
// Mixer or `ordered` is not exactly the mixer's current layer set.
[[nodiscard]] int set_layer_order(GradeGraph& g, int mixer, const std::vector<int>& ordered);

}  // namespace canvas::core::grade_graph