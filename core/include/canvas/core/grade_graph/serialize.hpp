#pragma once

// Project-file JSON round-trip for the node-grade graph (Phase 3). Qt-free;
// nlohmann-json is a system header (same usage as core/src/project/project.cpp).
//
// Format is string-enum based for forward compatibility: unknown enum strings
// map to the default value instead of aborting a project load, and unknown
// node kinds are SKIPPED (the stale node is dropped, its edges ignored, and the
// rest of the graph loads). Field names line up with the model (graph.hpp):
//
//   {"nodes":[{"id":I,"kind":"corrector","label":"..","bypass":B,"opacity":F,
//              "partner":P,"shared_source":S,"correct_mode":"lgg",
//              "lgg":{...},"cdl":{...},"key_mode":"add","blend":"normal"}],
//    "edges":[{"from":{"node",I,"type":"rgb","port":P},"to":{...}}]}
//
// The `grade` clip field is omitted entirely when a clip has no grade (see
// Clip::has_grade), so untouched clips in legacy projects stay byte-identical
// on save.

#include "canvas/core/grade_graph/graph.hpp"

#include <nlohmann/json.hpp>

namespace canvas::core::grade_graph {

// Serializes the whole graph. An empty graph round-trips as {"nodes":[],"edges":[]}.
[[nodiscard]] nlohmann::json grade_graph_to_json(const GradeGraph& g);

// Deserializes; never throws for unknown enum strings (they take defaults) but
// rejects structurally broken input (throws nlohmann type_error) exactly like
// the rest of the project loader does. Unknown node kinds are skipped.
[[nodiscard]] GradeGraph grade_graph_from_json(const nlohmann::json& j);

}  // namespace canvas::core::grade_graph