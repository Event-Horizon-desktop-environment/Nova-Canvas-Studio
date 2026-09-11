#pragma once

// Op registry for grade_graph's per-node op slot (Phase 6b). Qt-free, same
// culture as the model/evaluator it sits beside (grade_graph/).
//
// Each image node carries ONE op key — `correct_mode`, of type OpKind — plus
// its parameter structs (lgg/cdl/offset/curves). This module owns the seam
// that Phase 7 widens into the full effect taxonomy:
//
//   - op_apply       — the pointwise law dispatch (the evaluator and GPU path
//                      call this; the LGG/CDL/curves math itself stays in
//                      colorsci/). Effect ops (blur/glow/grain) register as
//                      new OpKind members + apply functions in Phase 7; the
//                      serial and Layer Mixer topology never changes.
//   - op_is_identity — the OpenFX "IsIdentity" fast-path: with the node's
//                      CURRENT params the op is a byte-identical no-op, so a
//                      host may skip it and copy the input. Provable here
//                      because identity is exact-by-params for every current
//                      kind (default LGG/CDL/offset/empty curves are the
//                      identity laws of their colorsci counterparts).
//   - op_name/from   — the single round-trip + UI name table; serialize.cpp
//                      delegates here so enum<->string never drifts. Unknown
//                      strings map to kIdentity (forward-compatible load).
//
// Naming note: the Node field is still called `correct_mode` and the old enum
// spelling `CorrectMode` is preserved as a deprecated alias, so Phase 3-era
// project files and the existing Qt tree keep loading/compiling unchanged.
// New code should speak OpKind.

#include "canvas/core/grade_graph/graph.hpp"

#include <string>

namespace canvas::core::grade_graph {

// Pointwise application of the node's op to one RGB sample (the "corrected"
// operand the evaluator then blends by key*opacity).
[[nodiscard]] colorsci::RGBF op_apply(const Node& node, const colorsci::RGBF& in);

// True when the node's CURRENT params make its op a byte-identical no-op
// (skip-copy fast path). Note: bypass is handled by the evaluator, not here —
// this answers "is the op itself identity", not "is the node disabled".
[[nodiscard]] bool op_is_identity(const Node& node);

// Round-trip names (identical strings to the Phase 3 project file: "identity",
// "lgg", "cdl", "curves"). op_from_name returns kIdentity for unknown strings.
[[nodiscard]] const char* op_name(OpKind kind);
[[nodiscard]] OpKind op_from_name(const std::string& name);

}  // namespace canvas::core::grade_graph