#include "canvas/core/grade_graph/edit.hpp"

#include <algorithm>
#include <set>
#include <string>

namespace canvas::core::grade_graph {

namespace {

// Same-type check shared by every wiring law: both ends must carry the same
// pipe AND the from end must actually be an output of that pipe while the to
// end is an input. Port/target lookup and cycle/occupancy are the callers'.
bool is_legal_binding(const NodePorts& from_ports, PipeType type, int from_port,
                      const NodePorts& to_ports) {
    const bool to_holds_input = type == PipeType::kRgb
                                    ? to_ports.rgb_in > 0 || to_ports.unbounded_rgb_in
                                    : (type == PipeType::kKey ? to_ports.key_in > 0
                                                              : to_ports.channel_in > 0);
    if (!to_holds_input) {
        return false;
    }
    if (from_ports.rgb_out <= 0 && from_ports.key_out <= 0 && from_ports.channel_out <= 0) {
        return false;
    }
    if (type == PipeType::kRgb) {
        return from_ports.rgb_out > 0 && from_port == 0;
    }
    if (type == PipeType::kKey) {
        return from_ports.key_out > 0 && from_port == 0;
    }
    return from_ports.channel_out > 0 && from_port >= 0 && from_port < 3;
}

// Reverse-lookup of an edge's occupant on one destination pipe/port (nullptr
// when the port is free).
const Edge* occupant_on(const GradeGraph& g, PipeId to) {
    for (const Edge& e : g.edges()) {
        if (e.to.node == to.node && e.to.type == to.type && e.to.port == to.port) {
            return &e;
        }
    }
    return nullptr;
}

}  // namespace

NodePorts node_ports(NodeKind kind) {
    switch (kind) {
        case NodeKind::kCorrector: return {.rgb_in = 1, .rgb_out = 1, .key_in = 1, .key_out = 1};
        case NodeKind::kParallel: return {.rgb_in = 1, .rgb_out = 1, .key_in = 1, .key_out = 1};
        case NodeKind::kLayer: return {.rgb_in = 1, .rgb_out = 1, .key_in = 1, .key_out = 1};
        case NodeKind::kOutside: return {.rgb_in = 1, .rgb_out = 1, .key_out = 1};
        case NodeKind::kKeyMixer: return {.key_in = 2, .key_out = 1};
        case NodeKind::kSplitter: return {.rgb_in = 1, .channel_out = 3};
        case NodeKind::kCombiner: return {.rgb_out = 1, .channel_in = 3};
        case NodeKind::kParallelMixer: return {.rgb_in = 2, .rgb_out = 1};
        case NodeKind::kLayerMixer: return {.rgb_in = 1, .rgb_out = 1, .unbounded_rgb_in = true};
        case NodeKind::kOutput: return {.rgb_in = 1};
    }
    return {};
}

WireRule can_wire(const GradeGraph& g, PipeId from, PipeId to) {
    if (from.node < 0 || to.node < 0) {
        return {false, "invalid node"};
    }
    if (from.node == to.node) {
        return {false, "self wire"};
    }
    const std::size_t n = static_cast<std::size_t>(g.num_nodes());
    if (static_cast<std::size_t>(from.node) >= n || static_cast<std::size_t>(to.node) >= n) {
        return {false, "no such node"};
    }
    if (from.type != to.type) {
        return {false, "type mismatch"};
    }
    const NodePorts fp = node_ports(g.node(from.node).kind);
    const NodePorts tp = node_ports(g.node(to.node).kind);

    // Port existence on the destination. rgb_in special: fixed counts admit
    // ports [0, rgb_in); a Layer Mixer additionally admits ANY port >= its
    // fixed base count (layer slots 1..N).
    const bool port_exists = to.type == PipeType::kRgb
                                 ? to.port >= 0 &&
                                       (to.port < tp.rgb_in ||
                                        (tp.unbounded_rgb_in && to.port >= tp.rgb_in))
                                 : (to.type == PipeType::kKey
                                        ? to.port >= 0 && to.port < tp.key_in
                                        : to.port >= 0 && to.port < tp.channel_in);
    if (!port_exists) {
        return {false, "port missing"};
    }
    if (occupant_on(g, to) != nullptr) {
        return {false, "occupied"};
    }
    if (!is_legal_binding(fp, from.type, from.port, tp)) {
        return {false, "not a typed output"};
    }
    if (g.would_create_cycle(from, to)) {
        return {false, "cycle"};
    }
    return {true, "ok"};
}

int connect_or_replace(GradeGraph& g, PipeId from, PipeId to) {
    const WireRule rule = can_wire(g, from, to);
    if (!rule.ok) {
        if (std::string(rule.reason) != "occupied") {
            return -1;
        }
        const Edge* old = occupant_on(g, to);
        if (old == nullptr || !g.remove_edge(old->from, old->to)) {
            return -1;
        }
    }
    return g.add_edge(from, to);
}

int insert_branch(GradeGraph& g, NodeKind kind, int sink) {
    if (kind != NodeKind::kParallel && kind != NodeKind::kLayer) {
        return -1;
    }
    if (sink < 0 || static_cast<std::size_t>(sink) >= static_cast<std::size_t>(g.num_nodes())) {
        return -1;
    }
    // The sink must expose a feedable rgb input (a Key Mixer has none).
    const NodePorts sp = node_ports(g.node(sink).kind);
    if (!(sp.rgb_in > 0 || sp.unbounded_rgb_in)) {
        return -1;
    }
    const std::optional<int> base = g.rgb_source_of(sink);
    if (base && (*base < 0 || static_cast<std::size_t>(*base) >= static_cast<std::size_t>(g.num_nodes()))) {
        return -1;
    }
    // Both mixers need a real wired source on the sink (see edit.hpp): the
    // evaluator's A slot is the mixer's FIRST rgb edge, and a lone layer edge
    // would be promoted to base — skipping its key gate.
    if (!base) {
        return -1;
    }

    const int branch = g.add_node(kind);
    const int mixer = g.add_node(kind == NodeKind::kParallel ? NodeKind::kParallelMixer
                                                             : NodeKind::kLayerMixer);
    g.node(mixer).shared_source = *base;

    // The base->sink wire is stripped first and the mixer's rgb inputs wired
    // base-before-branch: the evaluator's A slot is the mixer's FIRST rgb
    // edge (WIRE-ORDER CONTRACT, see edit.hpp). All five wires below are
    // validated above (bounds, arity, acyclicity), so the guard only ever
    // trips on internal inconsistency.
    if (!g.remove_edge({*base, PipeType::kRgb, 0}, {sink, PipeType::kRgb, 0}) ||
        g.add_edge({*base, PipeType::kRgb, 0}, {branch, PipeType::kRgb, 0}) < 0 ||
        g.add_edge({*base, PipeType::kRgb, 0}, {mixer, PipeType::kRgb, 0}) < 0 ||
        g.add_edge({branch, PipeType::kRgb, 0}, {mixer, PipeType::kRgb, 1}) < 0 ||
        g.add_edge({mixer, PipeType::kRgb, 0}, {sink, PipeType::kRgb, 0}) < 0) {
        return -1;
    }
    return mixer;
}

int add_layer(GradeGraph& g, int mixer, int layer) {
    if (mixer < 0 || layer < 0) {
        return -1;
    }
    const std::size_t n = static_cast<std::size_t>(g.num_nodes());
    if (static_cast<std::size_t>(mixer) >= n || static_cast<std::size_t>(layer) >= n) {
        return -1;
    }
    if (g.node(mixer).kind != NodeKind::kLayerMixer) {
        return -1;
    }
    // The mixer's FIRST rgb edge is its base (the evaluator's ins[0]); every
    // subsequent one is a layer. Highest-tracked port assigns the next free
    // slot; legacy port-0-only stacks (Phase 2 add_rgb_edge) still append at
    // port 1.
    int highest = 0;
    bool seen_base = false;
    for (const Edge& e : g.edges()) {
        if (e.to.node == mixer && e.to.type == PipeType::kRgb) {
            if (!seen_base) {
                seen_base = true;
                if (e.from.node == layer) {
                    return -1;  // already the base
                }
            } else {
                if (e.from.node == layer) {
                    return -1;  // already a layer
                }
                highest = std::max(highest, static_cast<int>(e.to.port));
            }
        }
    }
    const int port = highest + 1;
    return connect_or_replace(g, {layer, PipeType::kRgb, 0}, {mixer, PipeType::kRgb, port}) >= 0
               ? port
               : -1;
}

int set_layer_order(GradeGraph& g, int mixer, const std::vector<int>& ordered) {
    if (mixer < 0 || static_cast<std::size_t>(mixer) >= static_cast<std::size_t>(g.num_nodes())) {
        return -1;
    }
    if (g.node(mixer).kind != NodeKind::kLayerMixer) {
        return -1;
    }
    // The mixer's FIRST rgb edge is the base (the evaluator's ins[0]); every
    // edge after it is a layer. Tolerates legacy port-0-only stacks by
    // re-porting the layers to 1..N rather than refusing them.
    std::vector<std::pair<int, int>> layers;  // (port, source), layers only
    bool seen_base = false;
    for (const Edge& e : g.edges()) {
        if (e.to.node == mixer && e.to.type == PipeType::kRgb) {
            if (!seen_base) {
                seen_base = true;
            } else {
                layers.emplace_back(e.to.port, e.from.node);
            }
        }
    }
    if (layers.size() != ordered.size()) {
        return -1;
    }
    std::set<int> current;
    for (const auto& [port, src] : layers) {
        current.insert(src);
    }
    for (const int src : ordered) {
        if (!current.erase(src)) {
            return -1;  // duplicate or unknown layer
        }
    }
    if (!current.empty()) {
        return -1;
    }

    for (const auto& [port, src] : layers) {
        if (!g.remove_edge({src, PipeType::kRgb, port}, {mixer, PipeType::kRgb, port})) {
            return -1;
        }
    }
    for (std::size_t i = 0; i < ordered.size(); ++i) {
        if (g.add_edge({ordered[i], PipeType::kRgb, 0}, {mixer, PipeType::kRgb, static_cast<int>(i + 1)}) < 0) {
            return -1;
        }
    }
    return static_cast<int>(ordered.size());
}

}  // namespace canvas::core::grade_graph