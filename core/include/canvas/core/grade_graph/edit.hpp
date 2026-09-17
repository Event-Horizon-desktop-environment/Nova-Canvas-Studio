#pragma once

#include "canvas/core/grade_graph/graph.hpp"

namespace canvas::core::grade_graph {

struct WireRule {
    bool ok = false;
    const char* reason = "?";
};

inline constexpr int kUnboundedPorts = -1;

struct NodePorts {
    int rgb_in = 0;
    int rgb_out = 0;
    int key_in = 0;
    int key_out = 0;
    int channel_in = 0;
    int channel_out = 0;
    bool unbounded_rgb_in = false;
};

[[nodiscard]] NodePorts node_ports(NodeKind kind);

[[nodiscard]] WireRule can_wire(const GradeGraph& g, PipeId from, PipeId to);

[[nodiscard]] int connect_or_replace(GradeGraph& g, PipeId from, PipeId to);

[[nodiscard]] int insert_branch(GradeGraph& g, NodeKind kind, int sink);

[[nodiscard]] int add_layer(GradeGraph& g, int mixer, int layer);

[[nodiscard]] int set_layer_order(GradeGraph& g, int mixer, const std::vector<int>& ordered);

}