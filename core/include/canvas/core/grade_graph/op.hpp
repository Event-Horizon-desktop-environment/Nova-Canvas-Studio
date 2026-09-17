#pragma once

#include "canvas/core/grade_graph/graph.hpp"

#include <string>

namespace canvas::core::grade_graph {

[[nodiscard]] colorsci::RGBF op_apply(const Node& node, const colorsci::RGBF& in);

[[nodiscard]] bool op_is_identity(const Node& node);

[[nodiscard]] const char* op_name(OpKind kind);
[[nodiscard]] OpKind op_from_name(const std::string& name);

}
