// Phase 2 tests for the node-graph model + CPU evaluator
// (canvas/core/grade_graph). Exercises the dual-pipe signal model: serial
// correctors, key gating + passthrough, bypass/opacity, parallel mixer
// additivity, layer-mixer blend math + explicit stack order, outside
// partitioning, key-mixer modes, splitter/combiner round-trip, cycle
// rejection, and the exactly-one-output passthrough rule. Headless — links
// only canvas_core.

#include "canvas/core/grade_graph/graph.hpp"
#include "canvas/core/grade_graph/eval.hpp"
#include "canvas/core/grade_graph/serialize.hpp"
#include "canvas/core/export/grade_frame.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>

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

// Adds an identity corrector; its key_out is a uniform 1.0, handy as a source
// for key-math tests.
int add_identity(gg::GradeGraph& g, gg::NodeKind kind = gg::NodeKind::kCorrector) {
    const int id = g.add_node(kind);
    g.node(id).correct_mode = gg::CorrectMode::kIdentity;
    return id;
}

void test_passthrough_and_terminal() {
    gg::GradeGraph g;
    add_gain(g, 1.5f);
    const gg::EvalResult r = gg::evaluate_graph(g, src2x2(0.3f, 0.4f, 0.5f));
    check(!r.frame && r.error.empty(), "no output terminal -> tree inactive, passthrough");

    gg::GradeGraph g2;
    g2.add_node(gg::NodeKind::kOutput);
    const gg::EvalResult r2 = gg::evaluate_graph(g2, src2x2(0.3f, 0.4f, 0.5f));
    check(r2.frame && near(at_rgb(*r2.frame, 0, 0, 0), 0.3f) &&
              near(at_rgb(*r2.frame, 0, 0, 1), 0.4f) && near(at_rgb(*r2.frame, 0, 0, 2), 0.5f),
          "bare output terminal passes the source through");
}

void test_serial_chain() {
    // node0 = lift 0.4 (gamma 1, gain 1): out = x + 0.4(1-x)
    // node1 = cdl offset +0.2: out = x + 0.2
    gg::GradeGraph g;
    const int n0 = add_identity(g);
    g.node(n0).correct_mode = gg::CorrectMode::kLgg;
    g.node(n0).lgg.emplace();
    g.node(n0).lgg->lift_master = 0.4f;
    const int n1 = add_identity(g);
    g.node(n1).correct_mode = gg::CorrectMode::kCdl;
    g.node(n1).cdl.emplace();
    g.node(n1).cdl->offset_r = 0.2f;
    g.node(n1).cdl->offset_g = 0.2f;
    g.node(n1).cdl->offset_b = 0.2f;
    const int out = g.add_node(gg::NodeKind::kOutput);
    check(g.add_rgb_edge(n0, n1) >= 0, "serial chain edge n0->n1 accepted");
    check(g.add_rgb_edge(n1, out) >= 0, "serial chain edge n1->out accepted");

    const gg::EvalResult r = gg::evaluate_graph(g, src2x2(0.3f, 0.4f, 0.5f));
    // lift: 0.3 + 0.4*0.7 = 0.58 ; then offset: 0.78 / 0.84 / 0.90.
    expect_pix(r, 0.78f, 0.84f, 0.90f, "serial chain: lift then offset compose");
}

void test_bypass_opacity() {
    gg::GradeGraph g;
    const int c = add_gain(g, 1.5f);
    g.node(c).opacity = 0.5f;
    const int out = g.add_node(gg::NodeKind::kOutput);
    g.add_rgb_edge(c, out);
    const gg::EvalResult r = gg::evaluate_graph(g, src2x2(0.3f, 0.4f, 0.5f));
    expect_pix(r, 0.375f, 0.5f, 0.625f, "opacity 0.5 -> half the gain step");

    gg::GradeGraph g2;
    const int c2 = add_gain(g2, 1.5f);
    g2.node(c2).bypass = true;
    const int o2 = g2.add_node(gg::NodeKind::kOutput);
    g2.add_rgb_edge(c2, o2);
    const gg::EvalResult r2 = gg::evaluate_graph(g2, src2x2(0.3f, 0.4f, 0.5f));
    expect_pix(r2, 0.3f, 0.4f, 0.5f, "bypassed corrector leaves the pixel unchanged");
}

gg::EvalResult keyed_run(gg::KeyMixMode mode, int feeders) {
    gg::GradeGraph g;
    const int a = add_identity(g);
    const int b = add_identity(g);
    const int km = g.add_node(gg::NodeKind::kKeyMixer);
    g.node(km).key_mode = mode;
    if (feeders >= 1) g.add_key_edge(a, km);
    if (feeders >= 2) g.add_key_edge(b, km);
    const int corr = add_gain(g, 1.5f);
    g.add_key_edge(km, corr);
    const int out = g.add_node(gg::NodeKind::kOutput);
    g.add_rgb_edge(corr, out);
    return gg::evaluate_graph(g, src2x2(0.3f, 0.4f, 0.5f));
}

void test_key_pipe() {
    // Feeders have uniform key_out = 1.0.
    expect_pix(keyed_run(gg::KeyMixMode::kAdd, 2), 0.45f, 0.6f, 0.75f,
               "key Add(1,1) -> full correction");
    expect_pix(keyed_run(gg::KeyMixMode::kSubtract, 2), 0.3f, 0.4f, 0.5f,
               "key Subtract(1,1) -> no correction");
    expect_pix(keyed_run(gg::KeyMixMode::kIntersect, 2), 0.45f, 0.6f, 0.75f,
               "key Intersect(1,1) -> full correction");
    expect_pix(keyed_run(gg::KeyMixMode::kInvert, 1), 0.3f, 0.4f, 0.5f,
               "key Invert(1) -> no correction");

    // Key propagation through a serial corrector: the feeder's key reaches the
    // mixer verbatim (key pipe is its own sub-graph).
    gg::GradeGraph g;
    const int feeder = add_identity(g);
    const int relay = add_identity(g);  // serial, passes the key through
    g.add_key_edge(feeder, relay);
    const int corr = add_gain(g, 1.5f);
    const int out = g.add_node(gg::NodeKind::kOutput);
    g.add_key_edge(relay, corr);
    g.add_rgb_edge(corr, out);
    const gg::EvalResult r = gg::evaluate_graph(g, src2x2(0.3f, 0.4f, 0.5f));
    expect_pix(r, 0.45f, 0.6f, 0.75f, "serial node passes its key through");
}

void test_parallel() {
    // partner gain 1.2, branch gain 0.5, both read the source; mixer = A+B-base.
    gg::GradeGraph g;
    const int partner = add_gain(g, 1.2f);
    const int branch = add_gain(g, 0.5f, gg::NodeKind::kParallel);
    const int mixer = g.add_node(gg::NodeKind::kParallelMixer);
    const int out = g.add_node(gg::NodeKind::kOutput);
    g.add_rgb_edge(partner, mixer);
    g.add_rgb_edge(branch, mixer);
    g.add_rgb_edge(mixer, out);
    const gg::EvalResult r = gg::evaluate_graph(g, src2x2(0.3f, 0.4f, 0.5f));
    // A=0.36,0.48,0.6 ; B=0.15,0.2,0.25 ; base=0.3,0.4,0.5 -> 0.21,0.28,0.35
    expect_pix(r, 0.21f, 0.28f, 0.35f, "parallel mixer adds branch contributions to the base");

    // An identical branch contributes zero (no exposure doubling).
    gg::GradeGraph g2;
    const int p2 = add_gain(g2, 1.2f);
    const int idb = add_gain(g2, 1.0f, gg::NodeKind::kParallel);
    const int m2 = g2.add_node(gg::NodeKind::kParallelMixer);
    const int o2 = g2.add_node(gg::NodeKind::kOutput);
    g2.add_rgb_edge(p2, m2);
    g2.add_rgb_edge(idb, m2);
    g2.add_rgb_edge(m2, o2);
    const gg::EvalResult r2 = gg::evaluate_graph(g2, src2x2(0.3f, 0.4f, 0.5f));
    expect_pix(r2, 0.36f, 0.48f, 0.6f, "identical branch adds zero, mirrors the partner");

    // Branches chained from a shared upstream: the base is subtracted once.
    gg::GradeGraph g3;
    const int src3 = add_gain(g3, 1.0f);
    const int pa3 = add_gain(g3, 1.2f);
    const int br3 = add_gain(g3, 0.5f, gg::NodeKind::kParallel);
    g3.add_rgb_edge(src3, pa3);
    g3.add_rgb_edge(src3, br3);
    const int m3 = g3.add_node(gg::NodeKind::kParallelMixer);
    g3.node(m3).shared_source = src3;
    const int o3 = g3.add_node(gg::NodeKind::kOutput);
    g3.add_rgb_edge(pa3, m3);
    g3.add_rgb_edge(br3, m3);
    g3.add_rgb_edge(m3, o3);
    const gg::EvalResult r3 = gg::evaluate_graph(g3, src2x2(0.3f, 0.4f, 0.5f));
    expect_pix(r3, 0.21f, 0.28f, 0.35f, "parallel with shared upstream subtracts the base once");
}

int add_offset_layer(gg::GradeGraph& g, float off, gg::BlendMode blend) {
    const int id = g.add_node(gg::NodeKind::kLayer);
    g.node(id).correct_mode = gg::CorrectMode::kCdl;
    g.node(id).cdl.emplace();
    g.node(id).cdl->offset_r = off;
    g.node(id).cdl->offset_g = off;
    g.node(id).cdl->offset_b = off;
    g.node(id).blend = blend;
    return id;
}

int add_gain_layer(gg::GradeGraph& g, float slope, gg::BlendMode blend) {
    const int id = g.add_node(gg::NodeKind::kLayer);
    g.node(id).correct_mode = gg::CorrectMode::kCdl;
    g.node(id).cdl.emplace();
    g.node(id).cdl->slope_r = slope;
    g.node(id).cdl->slope_g = slope;
    g.node(id).cdl->slope_b = slope;
    g.node(id).blend = blend;
    return id;
}

void test_layer_mixer() {
    // Base is explicit (gain-1.0 provider); single normal layer = base + 0.2.
    // The layer's corrected output replaces the base via normal blend.
    gg::GradeGraph g2;
    const int b2 = add_gain(g2, 1.0f);                       // explicit base = source
    const int l2 = add_offset_layer(g2, 0.2f, gg::BlendMode::kNormal);
    const int m2 = g2.add_node(gg::NodeKind::kLayerMixer);
    const int o2 = g2.add_node(gg::NodeKind::kOutput);
    g2.add_rgb_edge(b2, m2);   // port 0 = base
    g2.add_rgb_edge(l2, m2);   // port 1 = layer
    g2.add_rgb_edge(m2, o2);
    const gg::EvalResult r2 = gg::evaluate_graph(g2, src2x2(0.3f, 0.4f, 0.5f));
    expect_pix(r2, 0.5f, 0.6f, 0.7f, "layer normal replaces the base with the layer");

    // Multiply blend: out = base * layer.
    gg::GradeGraph g3;
    const int b3 = add_gain(g3, 1.0f);
    const int l3 = add_gain_layer(g3, 0.4f, gg::BlendMode::kMultiply);
    const int m3 = g3.add_node(gg::NodeKind::kLayerMixer);
    const int o3 = g3.add_node(gg::NodeKind::kOutput);
    g3.add_rgb_edge(b3, m3);
    g3.add_rgb_edge(l3, m3);
    g3.add_rgb_edge(m3, o3);
    const gg::EvalResult r3 = gg::evaluate_graph(g3, src2x2(0.3f, 0.4f, 0.5f));
    // layer = 0.12,0.16,0.20 ; out = base*layer = 0.036,0.064,0.1
    expect_pix(r3, 0.036f, 0.064f, 0.1f, "layer multiply computes base * layer");

    // Layer opacity 0.5 blends half the delta.
    gg::GradeGraph g4;
    const int b4 = add_gain(g4, 1.0f);
    const int l4 = add_offset_layer(g4, 0.2f, gg::BlendMode::kNormal);
    g4.node(l4).opacity = 0.5f;
    const int m4 = g4.add_node(gg::NodeKind::kLayerMixer);
    const int o4 = g4.add_node(gg::NodeKind::kOutput);
    g4.add_rgb_edge(b4, m4);
    g4.add_rgb_edge(l4, m4);
    g4.add_rgb_edge(m4, o4);
    const gg::EvalResult r4 = gg::evaluate_graph(g4, src2x2(0.3f, 0.4f, 0.5f));
    expect_pix(r4, 0.4f, 0.5f, 0.6f, "layer opacity 0.5 blends half the delta");

    // Explicit stack order: {base 0.5} -> normal +0.2 -> multiply 0.1.
    const auto stacked = [](bool normal_first) {
        gg::GradeGraph g;
        const int base = add_gain(g, 1.0f);
        const int norm = add_offset_layer(g, 0.2f, gg::BlendMode::kNormal);
        const int mul = add_gain_layer(g, 0.1f, gg::BlendMode::kMultiply);
        const int mix = g.add_node(gg::NodeKind::kLayerMixer);
        const int out = g.add_node(gg::NodeKind::kOutput);
        g.add_rgb_edge(base, mix);
        if (normal_first) {
            g.add_rgb_edge(norm, mix);
            g.add_rgb_edge(mul, mix);
        } else {
            g.add_rgb_edge(mul, mix);
            g.add_rgb_edge(norm, mix);
        }
        g.add_rgb_edge(mix, out);
        return gg::evaluate_graph(g, src2x2(0.5f, 0.5f, 0.5f));
    };
    // normal-then-multiply: acc 0.5 -> 0.7 -> 0.7 * (0.1*0.5) = 0.035
    expect_pix(stacked(true), 0.035f, 0.035f, 0.035f, "stack: normal then multiply");
    // multiply-then-normal: acc 0.5 -> 0.05 -> 0.7 (normal replaces)
    expect_pix(stacked(false), 0.7f, 0.7f, 0.7f, "stack: multiply then normal");
}

void test_outside() {
    // partner gain 1.2 (no key -> uniform 1), outside gain 0.2 with
    // partner set: outside key = 1 - 1 = 0 -> its contribution is zero.
    gg::GradeGraph g;
    const int partner = add_gain(g, 1.2f);
    const int outside = add_gain(g, 0.2f, gg::NodeKind::kOutside);
    g.node(outside).partner = partner;
    const int mixer = g.add_node(gg::NodeKind::kParallelMixer);
    const int out = g.add_node(gg::NodeKind::kOutput);
    g.add_rgb_edge(partner, mixer);
    g.add_rgb_edge(outside, mixer);
    g.add_rgb_edge(mixer, out);
    const gg::EvalResult r = gg::evaluate_graph(g, src2x2(0.3f, 0.4f, 0.5f));
    expect_pix(r, 0.36f, 0.48f, 0.6f, "outside with empty partner key contributes nothing");

    // Give the partner a real key via an inverted tracker: feeder key 1 ->
    // invert -> 0 -> outside key = 1 - 0 = 1 -> outside applies in full.
    gg::GradeGraph g2;
    const int feeder = add_identity(g2);
    const int invert = g2.add_node(gg::NodeKind::kKeyMixer);
    g2.node(invert).key_mode = gg::KeyMixMode::kInvert;
    g2.add_key_edge(feeder, invert);
    const int partner2 = add_gain(g2, 0.9f);
    g2.add_key_edge(invert, partner2);
    const int outside2 = add_gain(g2, 1.5f, gg::NodeKind::kOutside);
    g2.node(outside2).partner = partner2;
    const int mixer2 = g2.add_node(gg::NodeKind::kParallelMixer);
    const int o2 = g2.add_node(gg::NodeKind::kOutput);
    g2.add_rgb_edge(partner2, mixer2);
    g2.add_rgb_edge(outside2, mixer2);
    g2.add_rgb_edge(mixer2, o2);
    const gg::EvalResult r2 = gg::evaluate_graph(g2, src2x2(0.3f, 0.4f, 0.5f));
    // partner is keyed 0 (no change), outside's partition (1-0)=1 applies in
    // full via gain 1.5; A + B - base = 0.3 + 0.45 - 0.3 = 0.45 (r channel).
    expect_pix(r2, 0.45f, 0.6f, 0.75f, "inverted partner key flips the partition");
}

void test_splitter_combiner() {
    gg::GradeGraph g;
    const int split = g.add_node(gg::NodeKind::kSplitter);
    const int comb = g.add_node(gg::NodeKind::kCombiner);
    const int out = g.add_node(gg::NodeKind::kOutput);
    g.add_channel_edge(split, 0, comb, 0);
    g.add_channel_edge(split, 1, comb, 1);
    g.add_channel_edge(split, 2, comb, 2);
    g.add_rgb_edge(comb, out);
    const gg::EvalResult r = gg::evaluate_graph(g, src2x2(0.3f, 0.4f, 0.5f));
    expect_pix(r, 0.3f, 0.4f, 0.5f, "splitter/combiner round-trips all channels");

    // Missing channel wires read as zero.
    gg::GradeGraph g2;
    const int split2 = g2.add_node(gg::NodeKind::kSplitter);
    const int comb2 = g2.add_node(gg::NodeKind::kCombiner);
    const int o2 = g2.add_node(gg::NodeKind::kOutput);
    g2.add_channel_edge(split2, 0, comb2, 0);
    g2.add_rgb_edge(comb2, o2);
    const gg::EvalResult r2 = gg::evaluate_graph(g2, src2x2(0.3f, 0.4f, 0.5f));
    expect_pix(r2, 0.3f, 0.0f, 0.0f, "combiner without G/B wires reads zero on those ports");
}

void test_cycle_rejection() {
    gg::GradeGraph g;
    const int n0 = add_gain(g, 1.0f);
    const int n1 = add_gain(g, 1.0f);
    const int n2 = add_gain(g, 1.0f);
    const int out = g.add_node(gg::NodeKind::kOutput);
    check(g.add_rgb_edge(n0, n1) >= 0, "core chain edge accepted");
    check(g.add_rgb_edge(n1, n2) >= 0, "core chain edge accepted");
    check(g.add_rgb_edge(n2, out) >= 0, "core chain edge accepted");
    check(g.add_rgb_edge(out, n0) < 0, "output->n0 rejected (would close a cycle)");
    check(g.add_rgb_edge(n2, n0) < 0, "n2->n0 rejected (cycle)");
    check(g.add_rgb_edge(n0, n0) < 0, "self edge rejected");

    // The cycle guard spans pipes: a key edge back into an upstream node is
    // also rejected.
    const int km = g.add_node(gg::NodeKind::kKeyMixer);
    const int odd = add_identity(g);
    g.add_key_edge(odd, km);
    check(g.add_key_edge(km, odd) < 0, "key edge closing a cycle is rejected");

    // The graph is still valid without the rejected edges.
    const gg::EvalResult r = gg::evaluate_graph(g, src2x2(0.3f, 0.4f, 0.5f));
    expect_pix(r, 0.3f, 0.4f, 0.5f, "evaluation proceeds on the non-cyclic remainder");
}

void test_inactive_subgraph_ignored() {
    // A corrector with nothing downstream of it must not fight the active
    // path nor leave dangling state in the result.
    gg::GradeGraph g;
    add_gain(g, -3.0f);  // inactive: no path to the terminal
    const int c = add_gain(g, 1.5f);
    const int out = g.add_node(gg::NodeKind::kOutput);
    g.add_rgb_edge(c, out);
    const gg::EvalResult r = gg::evaluate_graph(g, src2x2(0.3f, 0.4f, 0.5f));
    expect_pix(r, 0.45f, 0.6f, 0.75f, "inactive subgraph is skipped entirely");
}

void test_blend_into() {
    // Unit-level blend checks on a 2-pixel buffer.
    std::vector<float> acc = {0.5f, 0.5f, 0.5f, 1.0f, 0.3f, 0.3f, 0.3f, 1.0f};
    std::vector<float> lay = {0.2f, 0.2f, 0.2f, 1.0f, 0.6f, 0.6f, 0.6f, 1.0f};
    std::vector<float> out(8, 0.0f);

    gg::blend_into(acc.data(), lay.data(), out.data(), 2, gg::BlendMode::kMultiply);
    check(near(out[0], 0.1f) && near(out[4], 0.18f), "blend multiply");
    gg::blend_into(acc.data(), lay.data(), out.data(), 2, gg::BlendMode::kScreen);
    check(near(out[0], 0.6f) && near(out[4], 0.72f), "blend screen");
    gg::blend_into(acc.data(), lay.data(), out.data(), 2, gg::BlendMode::kDifference);
    check(near(out[0], 0.3f) && near(out[4], 0.3f), "blend difference");
    gg::blend_into(acc.data(), lay.data(), out.data(), 2, gg::BlendMode::kNormal);
    check(near(out[0], 0.2f), "blend normal keeps the layer");
}

// --- Phase 3: project-file JSON round-trip -----------------------------------

void test_serialize_roundtrip() {
    gg::GradeGraph g;
    // node0: layer branch, LGG gain 0.75 with label + non-1 opacity
    const int layer = g.add_node(gg::NodeKind::kLayer);
    g.node(layer).correct_mode = gg::CorrectMode::kLgg;
    g.node(layer).lgg.emplace();
    g.node(layer).lgg->gain_master = 0.75f;
    g.node(layer).lgg->lift_r = 0.05f;
    g.node(layer).opacity = 0.9f;
    g.node(layer).label = "dim";
    // node1: CDL corrector feeding the key domain is not possible; key source:
    // node1 identity corrector (key_out = uniform 1).
    const int key_src = g.add_node(gg::NodeKind::kCorrector);
    g.node(key_src).correct_mode = gg::CorrectMode::kIdentity;
    // node2: key mixer (subtract) takes the identity's key
    const int key = g.add_node(gg::NodeKind::kKeyMixer);
    g.node(key).key_mode = gg::KeyMixMode::kSubtract;
    // node3: layer mixer stack (base + layer)
    const int mix = g.add_node(gg::NodeKind::kLayerMixer);
    g.node(mix).blend = gg::BlendMode::kScreen;
    // node4: output terminal
    const int out_n = g.add_node(gg::NodeKind::kOutput);

    check(g.add_rgb_edge(layer, mix) >= 0, "serialize: layer rgb -> mixer");
    check(g.add_key_edge(key_src, key) >= 0, "serialize: keysrc key -> keymixer in");
    check(g.add_key_edge(key, mix) >= 0, "serialize: keymixer key -> layer mixer key");
    check(g.add_rgb_edge(mix, out_n) >= 0, "serialize: mixer rgb -> output");

    const nlohmann::json j = gg::grade_graph_to_json(g);
    gg::GradeGraph g2 = gg::grade_graph_from_json(j);

    check(g2.num_nodes() == g.num_nodes(), "serialize: node count survives");
    check(g2.edges().size() == g.edges().size(), "serialize: edge count survives");

    bool saw_layer = false;
    for (int i = 0; i < static_cast<int>(g2.num_nodes()); ++i) {
        const gg::Node& n = g2.node(i);
        if (!n.lgg && !n.cdl) continue;
        saw_layer = n.kind == gg::NodeKind::kLayer && n.correct_mode == gg::CorrectMode::kLgg &&
                    n.lgg && near(n.lgg->gain_master, 0.75f) &&
                    near(n.lgg->lift_r, 0.05f) && near(n.opacity, 0.9f) &&
                    n.label == "dim";
        check(saw_layer, "serialize: lgg params + opacity + label + kind survive");
    }
    check(saw_layer, "serialize: layer node found after load");
    check(g2.node(key).key_mode == gg::KeyMixMode::kSubtract, "serialize: key_mode survives");
    check(g2.node(mix).blend == gg::BlendMode::kScreen, "serialize: layer blend survives");
    check(g2.terminal() == out_n, "serialize: output terminal survives");

    // Empty graph round-trips as empty.
    const nlohmann::json je = gg::grade_graph_to_json(gg::GradeGraph{});
    const gg::GradeGraph ge = gg::grade_graph_from_json(je);
    check(ge.num_nodes() == 0 && ge.edges().empty(), "serialize: empty graph round-trip");

    // Tolerant load: unknown node kind skipped, unknown enum strings default.
    nlohmann::json nodes = nlohmann::json::array();
    nodes.push_back({{"id", 0}, {"kind", "hologram"}});
    nlohmann::json n1;
    n1["id"] = 1;
    n1["kind"] = "corrector";
    n1["correct_mode"] = "quantum";
    n1["blend"] = "warp";
    n1["key_mode"] = "warp";
    nodes.push_back(n1);
    nlohmann::json edges = nlohmann::json::array();
    nlohmann::json e1;
    e1["from"] = {{"node", 0}, {"type", "rgb"}, {"port", 0}};
    e1["to"] = {{"node", 1}, {"type", "rgb"}, {"port", 0}};
    edges.push_back(e1);
    nlohmann::json tolerant;
    tolerant["nodes"] = nodes;
    tolerant["edges"] = edges;
    const gg::GradeGraph gt = gg::grade_graph_from_json(tolerant);
    check(gt.num_nodes() == 1, "serialize: unknown node kind skipped");
    check(gt.node(0).correct_mode == gg::CorrectMode::kIdentity &&
              gt.node(0).blend == gg::BlendMode::kNormal &&
              gt.node(0).key_mode == gg::KeyMixMode::kAdd,
          "serialize: unknown enum strings fall back to defaults");
    check(gt.edges().empty(), "serialize: edge to skipped node dropped");
}

// --- Phase 3: renderer frame application (uint8 <-> FrameF glue) -------------

void test_apply_grade_to_frame() {
    // Source pixels (2x2 RGBA): aim for byte-exact half-gain round-trips.
    canvas::core::VideoFrame src;
    src.width = 2;
    src.height = 2;
    src.stride = 8;
    src.rgba = {40, 80, 160, 255, 10, 20, 30, 255, 90, 140, 200, 255, 0, 128, 255, 255};

    gg::GradeGraph g;
    const int corr = g.add_node(gg::NodeKind::kCorrector);
    g.node(corr).correct_mode = gg::CorrectMode::kLgg;
    g.node(corr).lgg.emplace();
    g.node(corr).lgg->gain_master = 0.5f;
    const int out_n = g.add_node(gg::NodeKind::kOutput);
    check(g.add_rgb_edge(corr, out_n) >= 0, "grade-frame: wire tree");

    canvas::core::VideoFramePtr graded = canvas::core::apply_grade_to_frame(src, g);
    check(graded != nullptr, "grade-frame: graded frame produced");
    if (graded) {
        check(near(graded->rgba[0], 20u) && near(graded->rgba[1], 40u) &&
                  near(graded->rgba[2], 80u),
              "grade-frame: gain 0.5 halves RGB bytes");
        check(graded->rgba[3] == 255u, "grade-frame: alpha rides along");
        // Byte-exact values [0,255], [128,255] -> [0,128], [64,255] at 0.5 gain.
        check(graded->rgba[12] == 0u && graded->rgba[13] == 64u,
              "grade-frame: byte-exact 0/128 -> 0/64");
        check(graded->width == 2 && graded->height == 2 && graded->stride == 8,
              "grade-frame: geometry preserved");
    }

    // No terminal => passthrough => nullptr, caller keeps the decoder frame.
    gg::GradeGraph bare;
    (void)bare.add_node(gg::NodeKind::kCorrector);
    canvas::core::VideoFramePtr untouched = canvas::core::apply_grade_to_frame(src, bare);
    check(untouched == nullptr, "grade-frame: unwired tree returns nullptr (passthrough)");

    // An empty graph (the "no grade" reporter state) is also passthrough.
    canvas::core::VideoFramePtr none = canvas::core::apply_grade_to_frame(src, gg::GradeGraph{});
    check(none == nullptr, "grade-frame: empty graph is passthrough");
}

}  // namespace

int main() {
    test_passthrough_and_terminal();
    test_serial_chain();
    test_bypass_opacity();
    test_key_pipe();
    test_parallel();
    test_layer_mixer();
    test_outside();
    test_splitter_combiner();
    test_cycle_rejection();
    test_inactive_subgraph_ignored();
    test_blend_into();
    test_serialize_roundtrip();
    test_apply_grade_to_frame();

    if (failures == 0) {
        std::printf("ALL TESTS PASSED\n");
        return 0;
    }
    std::fprintf(stderr, "%d FAILURES\n", failures);
    return 1;
}