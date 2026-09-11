#include "canvas/core/grade_graph/graph.hpp"

#include <cstddef>
#include <queue>
#include <set>

namespace canvas::core::grade_graph {

int GradeGraph::add_node(NodeKind kind) {
    Node n;
    n.id = static_cast<int>(nodes_.size());
    n.kind = kind;
    nodes_.push_back(n);
    return n.id;
}

Node& GradeGraph::node(int id) {
    return nodes_.at(static_cast<std::size_t>(id));
}

const Node& GradeGraph::node(int id) const {
    return nodes_.at(static_cast<std::size_t>(id));
}

int GradeGraph::add_edge(PipeId from, PipeId to) {
    return would_create_cycle(from, to)
               ? -1
               : [&] {
                     edges_.push_back({from, to});
                     return static_cast<int>(edges_.size()) - 1;
                 }();
}

int GradeGraph::add_rgb_edge(int from, int to) {
    return add_edge({from, PipeType::kRgb, 0}, {to, PipeType::kRgb, 0});
}

int GradeGraph::add_key_edge(int from, int to) {
    return add_edge({from, PipeType::kKey, 0}, {to, PipeType::kKey, 0});
}

int GradeGraph::add_channel_edge(int from, int port, int to, int port2) {
    return add_edge({from, PipeType::kChannel, port}, {to, PipeType::kChannel, port2});
}

bool GradeGraph::remove_edge(PipeId from, PipeId to) {
    for (std::size_t i = 0; i < edges_.size(); ++i) {
        const Edge& e = edges_[i];
        if (e.from.node == from.node && e.from.type == from.type && e.from.port == from.port &&
            e.to.node == to.node && e.to.type == to.type && e.to.port == to.port) {
            edges_.erase(edges_.begin() + static_cast<std::ptrdiff_t>(i));
            return true;
        }
    }
    return false;
}

bool GradeGraph::would_create_cycle(PipeId from, PipeId to) const {
    // Adding from->to creates a cycle iff a path already runs to -> ... -> from.
    // Walk every wire (all three pipe types) with an explicit DFS stack.
    std::vector<int> stack{to.node};
    std::set<int> seen;
    while (!stack.empty()) {
        const int cur = stack.back();
        stack.pop_back();
        if (!seen.insert(cur).second) continue;
        if (cur == from.node) return true;
        for (const Edge& e : edges_) {
            if (e.from.node == cur) stack.push_back(e.to.node);
        }
    }
    return false;
}

int GradeGraph::terminal() const {
    for (const Node& n : nodes_) {
        if (n.kind == NodeKind::kOutput) return n.id;
    }
    return -1;
}

std::optional<int> GradeGraph::rgb_source_of(int node) const {
    for (const Edge& e : edges_) {
        if (e.to.node == node && e.to.type == PipeType::kRgb) return e.from.node;
    }
    return std::nullopt;
}

std::vector<std::pair<int, int>> GradeGraph::key_sources_of(int node) const {
    std::vector<std::pair<int, int>> out;
    for (const Edge& e : edges_) {
        if (e.to.node == node && e.to.type == PipeType::kKey) {
            out.emplace_back(e.to.port, e.from.node);
        }
    }
    return out;
}

std::vector<std::pair<int, int>> GradeGraph::channel_sources_of(int node) const {
    std::vector<std::pair<int, int>> out;
    for (const Edge& e : edges_) {
        if (e.to.node == node && e.to.type == PipeType::kChannel) {
            out.emplace_back(e.to.port, e.from.node);
        }
    }
    return out;
}

std::size_t GradeGraph::num_nodes() const {
    return nodes_.size();
}

const std::vector<Edge>& GradeGraph::edges() const {
    return edges_;
}

void GradeGraph::clear() {
    nodes_.clear();
    edges_.clear();
}

}  // namespace canvas::core::grade_graph