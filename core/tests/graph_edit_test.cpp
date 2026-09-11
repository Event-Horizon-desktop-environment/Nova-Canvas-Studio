// Phase 6 graph-EDIT law tests (core/grade_graph/edit.hpp). Exercises the
// connection-validity law (type match, port existence, single-owner ports,
// cycle rejection), port replace-wiring, Parallel/Layer branch insertion with
// auto-created mixers (identity branch == evaluator no-op), layer-stack
// append/reorder (links preserved by construction), and the raw add_edge /
// remove_edge model additions the laws sit on. Headless — links only
// canvas_core.

#include "canvas/core/grade_graph/graph.hpp"
#include "canvas/core/grade_graph/eval.hpp"
#include "canvas/core/grade_graph/edit.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <set>
#include <vector>

namespace gg = canvas::core::grade_graph;

namespace {

int failures = 0;

void check(bool cond, const char* what) {
    if (!cond) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++failures;
    } else {
        std::printf("ok:   %s\n", what);
    }
}

constexpr float kClose = 1e-4f;

bool near(float a, float b) {
    return std::fabs(a - b) < kClose;
}

float at_rgb(const gg::FrameF& f, int x, int y, int c) {
    return f.rgba[static_cast<std::size_t>(y * f.w + x) * 4u + c];
}

gg::FrameF src2x2(float r, float g, float b) {
    return gg::FrameF::filled(2, 2, r, g, b);
}

void expect_pix(const gg::EvalResult& r, float er, float eg, float eb, const char* what) {
    if (!r.frame) {
        check(false, what);
        return;
    }
    const gg::FrameF& f = *r.frame;
    const bool ok = near(at_rgb(f, 0, 0, 0), er) && near(at_rgb(f, 0, 0, 1), eg) &&
                    near(at_rgb(f, 0, 0, 2), eb);
    check(ok, what);
}

// Adds an LGG corrector (gamma=1: out = gain * in).
int add_gain(gg::GradeGraph& g, float gain, gg::NodeKind kind = gg::NodeKind::kCorrector) {
    const int id = g.add_node(kind);
    g.node(id).correct_mode = gg::CorrectMode::kLgg;
    g.node(id).lgg.emplace();
    g.node(id).lgg->gain_master = gain;
    return id;
}

int add_identity(gg::GradeGraph& g, gg::NodeKind kind = gg::NodeKind::kCorrector) {
    const int id = g.add_node(kind);
    g.node(id).correct_mode = gg::CorrectMode::kIdentity;
    return id;
}

bool has_edge_toward(const gg::GradeGraph& g, int to_node, gg::PipeType type, int to_port,
                     int from_node) {
    for (const gg::Edge& e : g.edges()) {
        if (e.to.node == to_node && e.to.type == type && e.to.port == to_port &&
            e.from.node == from_node) {
            return true;
        }
    }
    return false;
}

int find_kind(const gg::GradeGraph& g, gg::NodeKind kind) {
    for (int i = 0; i < static_cast<int>(g.num_nodes()); ++i) {
        if (g.node(i).kind == kind) return i;
    }
    return -1;
}

// ---- Port table -------------------------------------------------------------

void test_node_ports() {
    const gg::NodePorts c = gg::node_ports(gg::NodeKind::kCorrector);
    check(c.rgb_in == 1 && c.rgb_out == 1 && c.key_in == 1 && c.key_out == 1 && !c.unbounded_rgb_in,
          "corrector: one rgb in/out, one key in/out");
    const gg::NodePorts lm = gg::node_ports(gg::NodeKind::kLayerMixer);
    check(lm.rgb_in == 1 && lm.rgb_out == 1 && lm.unbounded_rgb_in, "layer mixer: base + unbounded layer ins");
    const gg::NodePorts pm = gg::node_ports(gg::NodeKind::kParallelMixer);
    check(pm.rgb_in == 2 && pm.rgb_out == 1, "parallel mixer: A + B ins");
    const gg::NodePorts km = gg::node_ports(gg::NodeKind::kKeyMixer);
    check(km.key_in == 2 && km.key_out == 1 && km.rgb_in == 0, "key mixer: two key ins, no rgb");
    const gg::NodePorts sp = gg::node_ports(gg::NodeKind::kSplitter);
    check(sp.rgb_in == 1 && sp.channel_out == 3, "splitter: rgb in, three channel outs");
    const gg::NodePorts cbn = gg::node_ports(gg::NodeKind::kCombiner);
    check(cbn.channel_in == 3 && cbn.rgb_out == 1, "combiner: three channel ins, rgb out");
    const gg::NodePorts oo = gg::node_ports(gg::NodeKind::kOutside);
    check(oo.key_in == 0 && oo.key_out == 1 && oo.rgb_in == 1, "outside: auto-wired key in, key out exposed");
    const gg::NodePorts po = gg::node_ports(gg::NodeKind::kOutput);
    check(po.rgb_in == 1 && po.rgb_out == 0 && po.key_in == 0, "output: single rgb in");
}

// ---- Connection validity ----------------------------------------------------

void test_can_wire() {
    gg::GradeGraph g;
    const int c0 = add_identity(g);
    const int c1 = add_identity(g);
    const int km = g.add_node(gg::NodeKind::kKeyMixer);
    const int split = g.add_node(gg::NodeKind::kSplitter);
    const int comb = g.add_node(gg::NodeKind::kCombiner);

    const auto rgb = [](int n) { return gg::PipeId{n, gg::PipeType::kRgb, 0}; };
    const auto key = [](int n) { return gg::PipeId{n, gg::PipeType::kKey, 0}; };

    const gg::WireRule ok = gg::can_wire(g, rgb(c0), rgb(c1));
    check(ok.ok && std::string(ok.reason) == "ok", "valid rgb wire admitted");
    const gg::WireRule mismatch = gg::can_wire(g, key(c0), rgb(c1));
    check(!mismatch.ok && std::string(mismatch.reason) == "type mismatch",
          "key out into rgb in refused (type mismatch)");
    check(std::string(gg::can_wire(g, rgb(c0), rgb(c0)).reason) == "self wire",
          "self wire refused");
    gg::GradeGraph empty;
    check(!gg::can_wire(empty, rgb(0), rgb(1)).ok, "empty graph: wire refused");
    check(std::string(gg::can_wire(g, rgb(c0), rgb(99)).reason) == "no such node",
          "wire to missing node refused");
    check(std::string(gg::can_wire(g, rgb(-1), rgb(c0)).reason) == "invalid node",
          "negative node refused");
    check(std::string(gg::can_wire(g, key(km), gg::PipeId{c1, gg::PipeType::kKey, 1}).reason) ==
              "port missing",
          "key mixer key port 2 refused (only ports 0,1)");

    check(gg::can_wire(g, {split, gg::PipeType::kChannel, 0}, {comb, gg::PipeType::kChannel, 0}).ok,
          "splitter channel out -> combiner channel in admitted");
    check(std::string(gg::can_wire(g, rgb(split), rgb(comb)).reason) == "port missing",
          "rgb into a combiner refused (no rgb input)");
    check(std::string(gg::can_wire(g, rgb(split), rgb(c0)).reason) == "not a typed output",
          "splitter rgb port admitted as input target, but splitter has no rgb out");

    check(g.add_rgb_edge(c0, c1) >= 0, "core chain edge accepted");
    check(std::string(gg::can_wire(g, rgb(c1), rgb(c0)).reason) == "cycle", "closing a cycle refused");
    check(std::string(gg::can_wire(g, rgb(c0), rgb(c1)).reason) == "occupied",
          "second wire onto an occupied port refused");

    // Layer Mixer: base slot + any layer slot beyond it.
    const int lm = g.add_node(gg::NodeKind::kLayerMixer);
    check(gg::can_wire(g, rgb(c0), {lm, gg::PipeType::kRgb, 7}).ok,
          "layer mixer accepts a far layer port (unbounded)");
    check(std::string(gg::can_wire(g, key(c0), {lm, gg::PipeType::kRgb, 0}).reason) == "type mismatch",
          "key into the layer base refused");
}

// ---- Replace wiring + raw edge model additions ------------------------------

void test_connect_or_replace() {
    gg::GradeGraph g;
    const int a = add_identity(g);
    const int km = g.add_node(gg::NodeKind::kKeyMixer);
    const int corr = add_gain(g, 1.5f);

    const auto key_out = [](int n) { return gg::PipeId{n, gg::PipeType::kKey, 0}; };
    const auto key_in = [](int n) { return gg::PipeId{n, gg::PipeType::kKey, 0}; };

    const int first = gg::connect_or_replace(g, key_out(km), key_in(corr));
    check(first >= 0, "connect_or_replace: first key wire admitted");
    const int second = gg::connect_or_replace(g, key_out(a), key_in(corr));
    check(second >= 0, "connect_or_replace: second wire replaces the occupant");
    const auto ks = g.key_sources_of(corr);
    check(ks.size() == 1 && ks[0].second == a, "replaced occupant is gone, new source present");

    const std::size_t before = g.edges().size();
    check(gg::connect_or_replace(g, key_out(a), {corr, gg::PipeType::kRgb, 0}) == -1,
          "connect_or_replace: type mismatch refused");
    check(g.edges().size() == before, "connect_or_replace: nothing changed on refusal");
}

void test_edge_model_additions() {
    gg::GradeGraph g;
    const int a = add_gain(g, 1.0f);
    const int b = add_gain(g, 1.0f);
    check(g.add_rgb_edge(a, b) >= 0, "add_edge: chain edge accepted");
    check(!g.remove_edge({a, gg::PipeType::kKey, 0}, {b, gg::PipeType::kKey, 0}) &&
              g.edges().size() == 1,
          "remove_edge: wrong-pipe wire not removed");
    check(g.remove_edge({a, gg::PipeType::kRgb, 0}, {b, gg::PipeType::kRgb, 0}) &&
              g.edges().empty(),
          "remove_edge: exact wire removed");
    check(!g.remove_edge({a, gg::PipeType::kRgb, 0}, {b, gg::PipeType::kRgb, 0}),
          "remove_edge: second removal reports false");
}

// ---- Branch insertion (auto-created mixers) ---------------------------------

void test_insert_parallel_branch() {
    // Serial src(gain 1.2) -> sink(gain 0.5) -> out.
    gg::GradeGraph g;
    const int src = add_gain(g, 1.2f);
    const int sink = add_gain(g, 0.5f);
    const int ox = g.add_node(gg::NodeKind::kOutput);
    check(g.add_rgb_edge(src, sink) >= 0 && g.add_rgb_edge(sink, ox) >= 0, "serial chain built");

    const gg::EvalResult before = gg::evaluate_graph(g, src2x2(0.3f, 0.4f, 0.5f));
    expect_pix(before, 0.18f, 0.24f, 0.30f, "baseline serial result (0.3*1.2*0.5)");

    check(gg::insert_branch(g, gg::NodeKind::kCorrector, sink) == -1, "non-branch kind refused");
    const int mixer = gg::insert_branch(g, gg::NodeKind::kParallel, sink);
    check(mixer >= 0, "parallel branch inserted with auto-created mixer");
    const int branch = find_kind(g, gg::NodeKind::kParallel);
    check(branch >= 0 && g.node(mixer).kind == gg::NodeKind::kParallelMixer,
          "branch node + parallel mixer created");
    check(g.num_nodes() == 5, "insert grew the graph by exactly two nodes");

    check(!has_edge_toward(g, sink, gg::PipeType::kRgb, 0, src), "original src->sink wire severed");
    check(has_edge_toward(g, branch, gg::PipeType::kRgb, 0, src), "branch fed the shared source");
    check(has_edge_toward(g, mixer, gg::PipeType::kRgb, 0, src), "mixer A/base fed the shared source");
    check(has_edge_toward(g, mixer, gg::PipeType::kRgb, 1, branch), "mixer B fed the branch");
    check(has_edge_toward(g, sink, gg::PipeType::kRgb, 0, mixer), "mixer output feeds the sink");
    check(g.node(mixer).shared_source == src, "parallel mixer shares the source base");

    // Identity branch must reproduce the baseline exactly (A+B-base with A==B).
    const gg::EvalResult idle = gg::evaluate_graph(g, src2x2(0.3f, 0.4f, 0.5f));
    expect_pix(idle, 0.18f, 0.24f, 0.30f, "identity parallel branch is an evaluator no-op");

    // Give the branch a real correction (gain 0.5): out = src*0.5*0.5*... see
    // comment. branch = 0.5*srcOut; mixer = srcOut + branch - srcOut = branch;
    g.node(branch).correct_mode = gg::CorrectMode::kLgg;
    g.node(branch).lgg.emplace();
    g.node(branch).lgg->gain_master = 0.5f;
    const gg::EvalResult noted = gg::evaluate_graph(g, src2x2(0.3f, 0.4f, 0.5f));
    expect_pix(noted, 0.09f, 0.12f, 0.15f, "parallel branch exposes its delta over the base");

    // Refused when the sink has no wired source (chain head).
    gg::GradeGraph h;
    const int head = add_gain(h, 1.0f);
    const int ho = h.add_node(gg::NodeKind::kOutput);
    h.add_rgb_edge(head, ho);
    check(gg::insert_branch(h, gg::NodeKind::kParallel, head) == -1,
          "parallel branch over an unwired sink refused");
}

void test_insert_layer_branch_and_stack() {
    // Serial src(identity) -> sink(gain 1.5) -> out.
    gg::GradeGraph g;
    const int src = add_identity(g);
    const int sink = add_gain(g, 1.5f);
    const int ox = g.add_node(gg::NodeKind::kOutput);
    g.add_rgb_edge(src, sink);
    g.add_rgb_edge(sink, ox);

    const int mixer = gg::insert_branch(g, gg::NodeKind::kLayer, sink);
    check(mixer >= 0, "layer branch inserted with auto-created mixer");
    check(g.node(mixer).kind == gg::NodeKind::kLayerMixer, "mixer is a Layer Mixer");
    check(has_edge_toward(g, mixer, gg::PipeType::kRgb, 0, src), "layer base wired first (edge order)");
    int branch = find_kind(g, gg::NodeKind::kLayer);
    check(has_edge_toward(g, mixer, gg::PipeType::kRgb, 1, branch), "layer branch wired on port 1");

    const gg::EvalResult parked = gg::evaluate_graph(g, src2x2(0.3f, 0.4f, 0.5f));
    expect_pix(parked, 0.45f, 0.6f, 0.75f, "identity layer branch preserves the serial result");

    // A stacked layer over the same base, appended at the next free port.
    const int mul = add_gain(g, 0.5f, gg::NodeKind::kLayer);
    g.node(mul).blend = gg::BlendMode::kMultiply;
    const int port = gg::add_layer(g, mixer, mul);
    check(port == 2, "add_layer assigns the next free layer port");
    check(gg::add_layer(g, mixer, mul) == -1, "add_layer refuses an already-stacked layer");
    check(gg::add_layer(g, mixer, src) == -1, "add_layer refuses the base as a layer");

    // layer = 0.5*source over base(identity source 0.3): acc = 0.3 * 0.15 =
    // 0.045 (multiply) -> sink gain 1.5 -> 0.0675.
    const gg::EvalResult stacked = gg::evaluate_graph(g, src2x2(0.3f, 0.4f, 0.5f));
    expect_pix(stacked, 0.0675f, 0.12f, 0.1875f, "stacked multiply layer blends over the base");

    // Reorder: explicit base + two distinguishable layers on a uniform source.
    gg::GradeGraph r;
    const int rb = add_gain(r, 1.0f);  // base = source (0.5)
    const int norm = r.add_node(gg::NodeKind::kLayer);
    r.node(norm).correct_mode = gg::CorrectMode::kCdl;
    r.node(norm).cdl.emplace();
    r.node(norm).cdl->offset_r = 0.2f;
    r.node(norm).cdl->offset_g = 0.2f;
    r.node(norm).cdl->offset_b = 0.2f;
    r.node(norm).blend = gg::BlendMode::kNormal;
    const int mul2 = r.add_node(gg::NodeKind::kLayer);
    r.node(mul2).correct_mode = gg::CorrectMode::kCdl;
    r.node(mul2).cdl.emplace();
    r.node(mul2).cdl->slope_r = 0.1f;
    r.node(mul2).cdl->slope_g = 0.1f;
    r.node(mul2).cdl->slope_b = 0.1f;
    r.node(mul2).blend = gg::BlendMode::kMultiply;
    const int mx = r.add_node(gg::NodeKind::kLayerMixer);
    const int roof = r.add_node(gg::NodeKind::kOutput);
    r.add_rgb_edge(rb, mx);
    r.add_rgb_edge(norm, mx);
    r.add_rgb_edge(mul2, mx);
    r.add_rgb_edge(mx, roof);

    const std::size_t edges_before = r.edges().size();
    // base 0.5 -> norm 0.7 -> multiply 0.7*0.05 = 0.035.
    const gg::EvalResult a = gg::evaluate_graph(r, src2x2(0.5f, 0.5f, 0.5f));
    expect_pix(a, 0.035f, 0.035f, 0.035f, "stack (base + normal + multiply) top-to-bottom");

    const int order = gg::set_layer_order(r, mx, {mul2, norm});
    check(order == 2, "set_layer_order reorders layers (returns count)");
    check(r.edges().size() == edges_before, "set_layer_order preserves total link count");
    check(has_edge_toward(r, mx, gg::PipeType::kRgb, 1, mul2) &&
              has_edge_toward(r, mx, gg::PipeType::kRgb, 2, norm),
          "reorder compacts ports (multiply -> 1, normal -> 2)");
    check(has_edge_toward(r, mx, gg::PipeType::kRgb, 0, rb), "reorder leaves the base link untouched");

    // base 0.5 -> multiply 0.05 -> normal replaces -> 0.7.
    const gg::EvalResult b = gg::evaluate_graph(r, src2x2(0.5f, 0.5f, 0.5f));
    expect_pix(b, 0.7f, 0.7f, 0.7f, "stack after reorder (multiply then normal)");

    const std::size_t edges_before2 = r.edges().size();
    check(gg::set_layer_order(r, mx, {norm}) == -1, "reorder with a missing layer refused");
    check(gg::set_layer_order(r, mx, {mul2, mul2}) == -1, "reorder with a duplicate layer refused");
    check(gg::set_layer_order(r, mx, {norm, mul2, rb}) == -1, "reorder with an extra entry refused");
    check(gg::set_layer_order(r, roof, {norm, mul2}) == -1, "reorder targets a non-mixer refused");
    check(r.edges().size() == edges_before2, "refused reorders leave the wiring intact");
}

}  // namespace

int main() {
    test_node_ports();
    test_can_wire();
    test_connect_or_replace();
    test_edge_model_additions();
    test_insert_parallel_branch();
    test_insert_layer_branch_and_stack();

    if (failures == 0) {
        std::printf("ALL TESTS PASSED\n");
        return 0;
    }
    std::fprintf(stderr, "%d FAILURES\n", failures);
    return 1;
}