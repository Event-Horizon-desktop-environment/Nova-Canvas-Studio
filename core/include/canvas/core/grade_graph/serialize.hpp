#pragma once

#include "canvas/core/grade_graph/graph.hpp"

#include <nlohmann/json.hpp>

namespace canvas::core::grade_graph {

[[nodiscard]] nlohmann::json grade_graph_to_json(const GradeGraph& g);

[[nodiscard]] GradeGraph grade_graph_from_json(const nlohmann::json& j);

}
