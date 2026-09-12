// Phase 6a Porter–Duff compositing law tests (core/grade_graph/composite.hpp +
// the evaluator's Layer Mixer integration). Verifies the classic coverage
// coefficients for every operator, the separable blend functions, the
// over-with-blend equation (including the degenerate opaque case == legacy
// "replace"), premultiplied/straight boundaries, the Fusion additive↔
// subtractive knob, Disjoint alpha clamping, the source-alpha folding the
// Layer Mixer does with key*opacity, and JSON round-trip of the new node
// fields. Headless — links only canvas_core.

#include "canvas/core/grade_graph/graph.hpp"
#include "canvas/core/grade_graph/eval.hpp"
#include "canvas/core/grade_graph/composite.hpp"
#include "canvas/core/grade_graph/serialize.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <nlohmann/json.hpp>
#include <vector>

namespace gg = canvas::core::grade_graph;
using gg::BlendMode;
using gg::CompositeOp;

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

int add_gain(gg::GradeGraph& g, float gain, gg::NodeKind kind = gg::NodeKind::kCorrector) {
    const int id = g.add_node(kind);
    g.node(id).correct_mode = gg::CorrectMode::kLgg;
    g.node(id).lgg.emplace();
    g.node(id).lgg->gain_master = gain;
    return id;
}

float at(const gg::FrameF& f, int x, int y, int c) {
    return f.rgba[static_cast<std::size_t>(y * f.w + x) * 4u + c];
}

float alpha_of(const gg::FrameF& f, int x, int y) {
    return f.rgba[static_cast<std::size_t>(y * f.w + x) * 4u + 3];
}

// ---- law: coverage coefficients ----------------------------------------------

void test_coverage_coefficients() {
    const float as = 0.4f;
    const float ab = 0.3f;
    const auto c = [&](gg::CompositeOp op) { return gg::coverage_coefficients(op, as, ab); };

    const gg::CoverageCoefficients over = c(gg::CompositeOp::kOver);
    check(near(over.fs, 1.0f) && near(over.fb, 1.0f - as), "over: fs=1, fb=1-as");
    const gg::CoverageCoefficients in = c(gg::CompositeOp::kIn);
    check(near(in.fs, ab) && near(in.fb, 0.0f), "in: source clipped by backdrop alpha");
    const gg::CoverageCoefficients out = c(gg::CompositeOp::kOut);
    check(near(out.fs, 1.0f - ab) && near(out.fb, 0.0f), "out: source held out of backdrop");
    const gg::CoverageCoefficients atop = c(gg::CompositeOp::kAtop);
    check(near(atop.fs, ab) && near(atop.fb, 1.0f - as), "atop: partitioned coverage");
    const gg::CoverageCoefficients xor_ = c(gg::CompositeOp::kXor);
    check(near(xor_.fs, 1.0f - ab) && near(xor_.fb, 1.0f - as), "xor: both clear on overlap");
    const gg::CoverageCoefficients disjoint = c(gg::CompositeOp::kDisjoint);
    check(near(disjoint.fs, 1.0f) && near(disjoint.fb, 1.0f - as),
          "disjoint: over colors, clamped alpha");
    const gg::CoverageCoefficients mask = c(gg::CompositeOp::kMask);
    check(near(mask.fs, 0.0f) && near(mask.fb, as), "mask: backdrop under source alpha");
    const gg::CoverageCoefficients stencil = c(gg::CompositeOp::kStencil);
    check(near(stencil.fs, 0.0f) && near(stencil.fb, 1.0f - as), "stencil: hole cut by 1-as");
}

// ---- law: blend functions ----------------------------------------------------

void test_blend_channel() {
    const float bk = 0.5f;
    const float src = 0.2f;
    check(near(gg::blend_channel(BlendMode::kNormal, bk, src), 0.2f), "normal: source (identity)");
    check(near(gg::blend_channel(BlendMode::kScreen, bk, src), 0.6f), "screen");
    check(near(gg::blend_channel(BlendMode::kMultiply, bk, src), 0.1f), "multiply");
    check(near(gg::blend_channel(BlendMode::kOverlay, bk, src), 0.2f), "overlay (bk <= 0.5)");
    check(near(gg::blend_channel(BlendMode::kSoftLight, bk, src), 0.35f), "soft light (src <= 0.5)");
    check(near(gg::blend_channel(BlendMode::kSoftLight, 0.5f, 0.8f), 0.62426584f),
          "soft light (src > 0.5, mid backdrop)");
    check(near(gg::blend_channel(BlendMode::kSoftLight, 0.1f, 0.8f), 0.2776f),
          "soft light (src > 0.5, dark backdrop uses 16A-12 polynomial)");
    check(near(gg::blend_channel(BlendMode::kAdd, bk, src), 0.7f), "add");
    check(near(gg::blend_channel(BlendMode::kSubtract, bk, src), 0.3f), "subtract");
    check(near(gg::blend_channel(BlendMode::kDifference, bk, src), 0.3f), "difference");
}

// ---- law: opaque over degenerates to legacy replace ---------------------------

void test_opaque_over_replaces() {
    const gg::CompositeSample s =
        gg::composite_sample(0.5f, 0.5f, 0.5f, 1.0f, 0.2f, 0.2f, 0.2f, 1.0f,
                             gg::CompositeOp::kOver, BlendMode::kNormal, 0.0f);
    check(near(s.r, 0.2f) && near(s.a, 1.0f), "opaque over + normal == source, alpha 1");
    const gg::CompositeSample m =
        gg::composite_sample(0.5f, 0.5f, 0.5f, 1.0f, 0.2f, 0.2f, 0.2f, 1.0f,
                             gg::CompositeOp::kOver, BlendMode::kMultiply, 0.0f);
    check(near(m.r, 0.1f), "opaque over + multiply == blend result");
}

// ---- law: the non-over Porter–Duff operators ---------------------------------

void test_porter_duff_ops() {
    // Opaque: every operator resolves to a legible closed form.
    auto s = [](gg::CompositeOp op) {
        return gg::composite_sample(0.5f, 0.6f, 0.7f, 1.0f, 0.2f, 0.3f, 0.9f, 1.0f, op,
                                    BlendMode::kNormal, 0.0f);
    };
    const gg::CompositeSample in = s(gg::CompositeOp::kIn);
    check(near(in.r, 0.2f) && near(in.a, 1.0f), "opaque in: source clipped by full backdrop");
    const gg::CompositeSample out = s(gg::CompositeOp::kOut);
    check(near(out.r, 0.0f) && near(out.a, 0.0f), "opaque out: held out of full backdrop");
    const gg::CompositeSample atop = s(gg::CompositeOp::kAtop);
    check(near(atop.r, 0.2f) && near(atop.a, 1.0f), "opaque atop: source wins the overlap");
    const gg::CompositeSample xor_ = s(gg::CompositeOp::kXor);
    check(near(xor_.r, 0.0f) && near(xor_.a, 0.0f), "opaque xor: both cancel");
    const gg::CompositeSample mask = s(gg::CompositeOp::kMask);
    check(near(mask.r, 0.5f) && near(mask.a, 1.0f),
          "opaque mask: backdrop keeps its color under source alpha");
    const gg::CompositeSample stencil = s(gg::CompositeOp::kStencil);
    check(near(stencil.r, 0.0f) && near(stencil.a, 0.0f), "opaque stencil: full hole");

    // Semi-transparent in: source clipped to backdrop coverage.
    const gg::CompositeSample semi_in =
        gg::composite_sample(0.5f, 0.5f, 0.5f, 0.8f, 1.0f, 1.0f, 1.0f, 0.4f,
                             gg::CompositeOp::kIn, BlendMode::kNormal, 0.0f);
    check(near(semi_in.a, 0.4f * 0.8f) && near(semi_in.r, 1.0f),
          "semi in: alpha = as*ab, color = source");
}

// ---- law: W3C over-with-blend on semi-transparent pixels ----------------------

void test_semi_transparent_over() {
    // as=0.4, ab=0.8; source (1,1,1), backdrop (0.6,0.6,0.6):
    //   ao = 0.4 + 0.8*0.6 = 0.88
    //   co (premul) = 0.4*1 + 0.6*0.8*0.6 = 0.688 -> straight = 0.688/0.88 = 0.781818
    const gg::CompositeSample s =
        gg::composite_sample(0.6f, 0.6f, 0.6f, 0.8f, 1.0f, 1.0f, 1.0f, 0.4f,
                             gg::CompositeOp::kOver, BlendMode::kNormal, 0.0f);
    check(near(s.a, 0.88f), "semi over: alpha = as + ab(1-as)");
    check(near(s.r, 0.688f / 0.88f), "semi over: W3C blend equation, then unpremultiply");
    check(near(s.g, 0.688f / 0.88f) && near(s.b, 0.688f / 0.88f), "semi over: all channels equal");

    // Screen blend on the same pair: B = 1-(1-0.6)(1-1) = 1, but the backdrop
    // term (1-as)*ab*Cb = 0.288 still leaks, so the result is the same
    // premultiplied number as the normal path (0.688/0.88), not full white.
    const gg::CompositeSample scr =
        gg::composite_sample(0.6f, 0.6f, 0.6f, 0.8f, 1.0f, 1.0f, 1.0f, 0.4f,
                             gg::CompositeOp::kOver, BlendMode::kScreen, 0.0f);
    check(near(scr.r, 0.688f / 0.88f), "semi over + screen equals the W3C equation");
}

// ---- law: Disjoint alpha clamp + additive knob --------------------------------

void test_disjoint_and_additive() {
    const gg::CompositeSample over =
        gg::composite_sample(0.6f, 0.6f, 0.6f, 0.7f, 1.0f, 1.0f, 1.0f, 0.7f,
                             gg::CompositeOp::kOver, BlendMode::kNormal, 0.0f);
    const gg::CompositeSample disjoint =
        gg::composite_sample(0.6f, 0.6f, 0.6f, 0.7f, 1.0f, 1.0f, 1.0f, 0.7f,
                             gg::CompositeOp::kDisjoint, BlendMode::kNormal, 0.0f);
    check(near(over.a, 0.7f + 0.7f * 0.3f), "over: 0.91 alpha");
    check(near(disjoint.a, 1.0f), "disjoint: alpha clamped to min(1, as+ab)");
    // Same premultiplied color (0.826) unpremultiplied by its own alpha: the
    // straight values differ only because the alphas differ.
    check(near(disjoint.r * disjoint.a, over.r * over.a),
          "disjoint: premultiplied color identical to over");
    check(near(disjoint.r, 0.826f), "disjoint: straight color under clamped alpha");

    // Additive knob on opaque pixels: 0.2 (subtractive) -> 0.7 (premul sum).
    auto t = [](float g) {
        return gg::composite_sample(0.5f, 0.5f, 0.5f, 1.0f, 0.2f, 0.2f, 0.2f, 1.0f,
                                    gg::CompositeOp::kOver, BlendMode::kNormal, g);
    };
    check(near(t(0.0f).r, 0.2f), "additive 0: subtractive over (replace on opaque)");
    check(near(t(1.0f).r, 0.7f), "additive 1: premultiplied sum (brighter)");
    const gg::CompositeSample mid = t(0.5f);
    check(near(mid.r, 0.45f), "additive 0.5: linear mix of the two color laws");
}

// ---- public blend_into: opaque parity + real alpha now ------------------------

void test_blend_into() {
    std::vector<float> acc = {0.5f, 0.5f, 0.5f, 1.0f, 0.3f, 0.3f, 0.3f, 1.0f};
    std::vector<float> lay = {0.2f, 0.2f, 0.2f, 1.0f, 0.6f, 0.6f, 0.6f, 1.0f};
    std::vector<float> out(8, 0.0f);

    gg::blend_into(acc.data(), lay.data(), out.data(), 2, BlendMode::kMultiply);
    check(near(out[0], 0.1f) && near(out[4], 0.18f), "blend_into multiply (opaque parity)");
    gg::blend_into(acc.data(), lay.data(), out.data(), 2, BlendMode::kScreen);
    check(near(out[0], 0.6f) && near(out[4], 0.72f), "blend_into screen (opaque parity)");
    gg::blend_into(acc.data(), lay.data(), out.data(), 2, BlendMode::kDifference);
    check(near(out[0], 0.3f) && near(out[4], 0.3f), "blend_into difference (opaque parity)");
    gg::blend_into(acc.data(), lay.data(), out.data(), 2, BlendMode::kNormal);
    check(near(out[0], 0.2f), "blend_into normal keeps the layer (opaque)");
    check(near(out[3], 1.0f), "blend_into alpha recomputed to 1 on opaque inputs");

    // Semi-transparent: alpha genuinely recomputed now (was: copied from acc).
    std::vector<float> sacc = {0.5f, 0.5f, 0.5f, 0.8f};
    std::vector<float> slay = {0.2f, 0.2f, 0.2f, 0.4f};
    std::vector<float> sout(4, 0.0f);
    gg::blend_into(sacc.data(), slay.data(), sout.data(), 1, BlendMode::kNormal);
    check(near(sout[3], 0.88f), "blend_into: over alpha as+ab(1-as) = 0.88");
    check(near(sout[0], 0.32f / 0.88f), "blend_into: premultiplied over color");
}

// ---- evaluator integration -----------------------------------------------------

// base(gain 1) + layer(gain 0.5, kLayer) over a semi-transparent source: the
// mixer must composite premultiplied and build alpha, not just veil.
void test_layer_mixer_premultiplied() {
    gg::GradeGraph g;
    const int base = add_gain(g, 1.0f);
    const int layer = add_gain(g, 0.5f, gg::NodeKind::kLayer);
    const int mix = g.add_node(gg::NodeKind::kLayerMixer);
    const int ox = g.add_node(gg::NodeKind::kOutput);
    g.add_rgb_edge(base, mix);
    g.add_rgb_edge(layer, mix);
    g.add_rgb_edge(mix, ox);

    const gg::FrameF src = gg::FrameF::filled(2, 2, 0.8f, 0.6f, 0.4f, 0.5f);
    const gg::EvalResult r = gg::evaluate_graph(g, src);
    check(!!r.frame, "layer mixer evaluates a premultiplied stack");
    if (!r.frame) return;
    // as=0.5, ab=0.5: ao=0.75; co=0.5*0.4 + 0.5*0.5*0.8 = 0.4; straight=0.4/0.75.
    check(near(alpha_of(*r.frame, 0, 0), 0.75f), "premultiplied stack builds alpha (0.5 -> 0.75)");
    check(near(at(*r.frame, 0, 0, 0), 0.4f / 0.75f), "premultiplied stack darkens the composite");
    // Channel independence: green/blue follow the same law with their values.
    const float co_g = 0.5f * (0.6f * 0.5f) + 0.5f * 0.5f * 0.6f;
    check(near(at(*r.frame, 0, 0, 1), co_g / 0.75f), "premultiplied stack: green channel");
}

// key*opacity folds into coverage; on opaque media it must equal the legacy
// veil exactly (base + (layer-base)*eff), and alpha stays 1.
void test_layer_mixer_key_fold() {
    gg::GradeGraph g;
    const int base = add_gain(g, 1.0f);       // base = source (0.4)
    const int layer = add_gain(g, 2.0f, gg::NodeKind::kLayer);  // layer = 0.8
    g.node(layer).opacity = 0.5f;             // eff = 0.5
    const int mix = g.add_node(gg::NodeKind::kLayerMixer);
    const int ox = g.add_node(gg::NodeKind::kOutput);
    g.add_rgb_edge(base, mix);
    g.add_rgb_edge(layer, mix);
    g.add_rgb_edge(mix, ox);

    const gg::EvalResult r = gg::evaluate_graph(g, gg::FrameF::filled(2, 2, 0.4f, 0.4f, 0.4f));
    check(!!r.frame, "key-fold layer stack evaluates");
    if (!r.frame) return;
    check(near(at(*r.frame, 0, 0, 0), 0.6f), "opacity 0.5 veil parity (0.4 -> 0.6)");
    check(near(alpha_of(*r.frame, 0, 0), 1.0f), "opaque stack keeps alpha 1");
}

// composite_op kOut through the evaluator: the layer is held out of the opaque
// base, leaving transparent black (alpha builds from the mixer law).
void test_layer_mixer_composite_op() {
    gg::GradeGraph g;
    const int base = add_gain(g, 1.0f);
    const int layer = add_gain(g, 1.0f, gg::NodeKind::kLayer);
    g.node(layer).composite_op = gg::CompositeOp::kOut;
    const int mix = g.add_node(gg::NodeKind::kLayerMixer);
    const int ox = g.add_node(gg::NodeKind::kOutput);
    g.add_rgb_edge(base, mix);
    g.add_rgb_edge(layer, mix);
    g.add_rgb_edge(mix, ox);

    const gg::EvalResult r = gg::evaluate_graph(g, gg::FrameF::filled(2, 2, 0.8f, 0.8f, 0.8f));
    check(!!r.frame, "kOut layer stack evaluates");
    if (!r.frame) return;
    check(near(at(*r.frame, 0, 0, 0), 0.0f), "kOut values source out of pale backdrop");
    check(near(alpha_of(*r.frame, 0, 0), 0.0f), "kOut alpha cancels");
}

// additive=1 rides the same law through the evaluator (opaque source).
void test_layer_mixer_additive() {
    gg::GradeGraph g;
    const int base = add_gain(g, 1.0f);
    const int layer = add_gain(g, 2.0f, gg::NodeKind::kLayer);
    g.node(layer).additive = 1.0f;
    const int mix = g.add_node(gg::NodeKind::kLayerMixer);
    const int ox = g.add_node(gg::NodeKind::kOutput);
    g.add_rgb_edge(base, mix);
    g.add_rgb_edge(layer, mix);
    g.add_rgb_edge(mix, ox);

    const gg::EvalResult r = gg::evaluate_graph(g, gg::FrameF::filled(2, 2, 0.4f, 0.4f, 0.4f));
    check(!!r.frame, "additive layer stack evaluates");
    if (!r.frame) return;
    check(near(at(*r.frame, 0, 0, 0), 1.2f),
          "additive premul sum (0.4 + 0.8, scene-linear, unclamped)");
}

// ---- serialization ------------------------------------------------------------

void test_serialize_composite_fields() {
    gg::GradeGraph g;
    const int layer = g.add_node(gg::NodeKind::kLayer);
    g.node(layer).composite_op = gg::CompositeOp::kIn;
    g.node(layer).additive = 0.35f;

    const nlohmann::json j = gg::grade_graph_to_json(g);
    const gg::GradeGraph g2 = gg::grade_graph_from_json(j);
    check(g2.num_nodes() == 1, "serialize: node count survives");
    check(g2.node(0).composite_op == gg::CompositeOp::kIn, "serialize: composite_op survives");
    check(near(g2.node(0).additive, 0.35f), "serialize: additive survives");

    // Tolerant load: unknown composite_op string falls back to Over.
    nlohmann::json nodes = nlohmann::json::array();
    nlohmann::json n;
    n["id"] = 0;
    n["kind"] = "layer";
    n["composite_op"] = "magic";
    n["additive"] = 0.25f;
    nodes.push_back(n);
    const gg::GradeGraph gt = gg::grade_graph_from_json(nlohmann::json{{"nodes", nodes}});
    check(gt.node(0).composite_op == gg::CompositeOp::kOver, "serialize: bad op falls back to over");
    check(near(gt.node(0).additive, 0.25f), "serialize: additive still loads");
}

}  // namespace

int main() {
    test_coverage_coefficients();
    test_blend_channel();
    test_opaque_over_replaces();
    test_porter_duff_ops();
    test_semi_transparent_over();
    test_disjoint_and_additive();
    test_blend_into();
    test_layer_mixer_premultiplied();
    test_layer_mixer_key_fold();
    test_layer_mixer_composite_op();
    test_layer_mixer_additive();
    test_serialize_composite_fields();

    if (failures == 0) {
        std::printf("ALL TESTS PASSED\n");
        return 0;
    }
    std::fprintf(stderr, "%d FAILURES\n", failures);
    return 1;
}