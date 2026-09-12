// Phase 6b op-registry tests (core/grade_graph/op.hpp + op.cpp). Pins the
// three duties of the registry that Phase 7 widens into the effect taxonomy:
// pointwise dispatch parity with the colorsci laws (op_apply must be
// byte-identical to the old evaluator inline switch), the OFX-style identity
// fast-path (op_is_identity: exact-by-params for every current kind), and the
// single round-trip name table (op_name/op_from_name with the forward-
// compatible unknown->identity fallback). Also verifies the deprecated
// `CorrectMode` alias keeps the pre-6b spelling compiling and identical.
// Headless — links only canvas_core.

#include "canvas/core/grade_graph/graph.hpp"
#include "canvas/core/grade_graph/op.hpp"
#include "canvas/core/grade_graph/serialize.hpp"

#include <cstdio>
#include <cstdlib>
#include <nlohmann/json.hpp>
#include <string>

namespace gg = canvas::core::grade_graph;
namespace colorsci = canvas::core::colorsci;

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

constexpr float kClose = 1e-6f;

bool near(const colorsci::RGBF& a, const colorsci::RGBF& b) {
    return a.r > b.r - kClose && a.r < b.r + kClose && a.g > b.g - kClose && a.g < b.g + kClose &&
           a.b > b.b - kClose && a.b < b.b + kClose;
}

// ---- op_apply: parity with the colorsci laws ----------------------------------

void test_apply_identity_passthrough() {
    // CorrectMode::kIdentity is the default op slot of a fresh node.
    gg::Node n;
    const colorsci::RGBF in{0.2f, 0.6f, 0.9f};
    check(n.correct_mode == gg::OpKind::kIdentity, "fresh node defaults to identity op");
    check(near(gg::op_apply(n, in), in), "identity op is a pointwise passthrough");
}

void test_apply_lgg_parity() {
    colorsci::LGG p;
    p.gain_master = 1.5f;
    p.lift_b = -0.1f;

    gg::Node n;
    n.correct_mode = gg::OpKind::kLgg;  // (OpKind canonical spelling)
    n.lgg = p;

    const colorsci::RGBF in{0.1f, 0.4f, 0.8f};
    check(near(gg::op_apply(n, in), colorsci::apply_lgg(in, p)),
          "kLgg op_apply == colorsci::apply_lgg");
}

void test_apply_lgg_offset_parity() {
    colorsci::Offset off{0.05f, 0.1f, 0.0f, -0.1f};
    colorsci::LGG lgg_p;
    lgg_p.gamma_master = 1.3f;

    gg::Node n;
    n.correct_mode = gg::CorrectMode::kLgg;  // (deprecated alias still compiles)
    n.offset = off;
    n.lgg = lgg_p;

    const colorsci::RGBF in{0.7f, 0.3f, 0.2f};
    const colorsci::RGBF expected = colorsci::apply_lgg(colorsci::apply_offset(in, off), lgg_p);
    check(near(gg::op_apply(n, in), expected), "kLgg offset applied before LGG (parity)");
}

void test_apply_cdl_parity() {
    colorsci::Cdl c;
    c.slope_r = 1.2f;
    c.power_g = 0.8f;
    c.sat = 0.6f;

    gg::Node n;
    n.correct_mode = gg::OpKind::kCdl;
    n.cdl = c;

    const colorsci::RGBF in{0.8f, 0.5f, 0.1f};
    check(near(gg::op_apply(n, in), colorsci::apply_cdl(in, c)),
          "kCdl op_apply == colorsci::apply_cdl");
}

void test_apply_curves_parity() {
    colorsci::CurveParams c;
    c.channels[static_cast<int>(colorsci::CurveChannel::kRed)].push_back({0.5f, 0.75f});

    gg::Node n;
    n.correct_mode = gg::OpKind::kCurves;
    n.curves = c;

    const colorsci::RGBF in{0.25f, 0.5f, 0.75f};
    check(near(gg::op_apply(n, in), colorsci::apply_curves(in, c)),
          "kCurves op_apply == colorsci::apply_curves");
}

// ---- op_is_identity: the OFX IsIdentity fast-path ------------------------------

void test_is_identity_kinds() {
    gg::Node identity;
    check(gg::op_is_identity(identity), "identity op is identity");

    gg::Node lgg_default;
    lgg_default.correct_mode = gg::OpKind::kLgg;
    check(gg::op_is_identity(lgg_default), "lgg with absent params is identity");
    lgg_default.lgg.emplace();
    check(gg::op_is_identity(lgg_default), "lgg with default params is identity");
    lgg_default.offset.emplace();
    check(gg::op_is_identity(lgg_default), "lgg + default offset is identity");

    gg::Node lgg_hot;
    lgg_hot.correct_mode = gg::OpKind::kLgg;
    lgg_hot.lgg.emplace();
    lgg_hot.lgg->gain_master = 1.5f;
    check(!gg::op_is_identity(lgg_hot), "lgg with gain != 1 is not identity");
    lgg_hot.lgg->gain_master = 1.0f;
    lgg_hot.lgg->gamma_r = 2.0f;
    check(!gg::op_is_identity(lgg_hot), "lgg with per-channel gamma != 1 is not identity");
    lgg_hot.lgg->gamma_r = 1.0f;
    lgg_hot.offset.emplace();
    lgg_hot.offset->master = 0.2f;
    check(!gg::op_is_identity(lgg_hot), "non-default offset kills identity");

    gg::Node cdl_default;
    cdl_default.correct_mode = gg::OpKind::kCdl;
    check(gg::op_is_identity(cdl_default), "cdl with absent params is identity");
    cdl_default.cdl.emplace();
    check(gg::op_is_identity(cdl_default), "cdl with default params is identity");
    cdl_default.cdl->sat = 0.5f;
    check(!gg::op_is_identity(cdl_default), "cdl with sat != 1 is not identity");

    gg::Node curves_default;
    curves_default.correct_mode = gg::OpKind::kCurves;
    check(gg::op_is_identity(curves_default), "curves with absent params is identity");
    curves_default.curves.emplace();
    check(gg::op_is_identity(curves_default), "empty curves (identity soft clip) is identity");
    curves_default.curves->soft_clip.high_soft = 0.4f;
    check(!gg::op_is_identity(curves_default), "non-identity soft clip kills identity");
    curves_default.curves->soft_clip = colorsci::SoftClip{};
    curves_default.curves->channels[1].push_back({0.5f, 0.6f});
    check(!gg::op_is_identity(curves_default), "a control point kills identity");
}

// ---- op_name / op_from_name: the single round-trip table ------------------------

void test_names() {
    const gg::OpKind kinds[] = {gg::OpKind::kIdentity, gg::OpKind::kLgg, gg::OpKind::kCdl,
                                gg::OpKind::kCurves};
    const char* expect[] = {"identity", "lgg", "cdl", "curves"};
    for (int i = 0; i < 4; ++i) {
        const char* got = gg::op_name(kinds[i]);
        check(std::string(got) == expect[i], "op_name matches Phase 3 string");
        check(gg::op_from_name(got) == kinds[i], "op_from_name round-trips op_name");
    }
    // Uniqueness: naming a different kind with the same string would silently
    // corrupt project files.
    for (int i = 0; i < 4; ++i)
        for (int j = i + 1; j < 4; ++j)
            check(std::string(gg::op_name(kinds[i])) != gg::op_name(kinds[j]),
                  "op names are all distinct");
    check(gg::op_from_name("quantum") == gg::OpKind::kIdentity,
          "unknown op string falls back to identity");
    check(gg::op_from_name("") == gg::OpKind::kIdentity, "empty op string falls back to identity");
}

// ---- serialization delegates to the registry ------------------------------------

void test_serialize_delegates() {
    gg::GradeGraph g;
    const int n = g.add_node(gg::NodeKind::kCorrector);
    g.node(n).correct_mode = gg::OpKind::kCurves;

    const nlohmann::json j = gg::grade_graph_to_json(g);
    check(j["nodes"][0]["correct_mode"] == "curves",
          "project JSON carries the registry name for the op slot");
    const gg::GradeGraph g2 = gg::grade_graph_from_json(j);
    check(g2.node(0).correct_mode == gg::OpKind::kCurves, "op slot survives round-trip");

    // Tolerant load stays forward-compatible through op_from_name.
    nlohmann::json nodes = nlohmann::json::array();
    nodes.push_back(nlohmann::json{{"id", 0}, {"kind", "corrector"}, {"correct_mode", "ocr"}});
    const gg::GradeGraph gt = gg::grade_graph_from_json(nlohmann::json{{"nodes", nodes}});
    check(gt.node(0).correct_mode == gg::OpKind::kIdentity, "unknown op string loads as identity");
}

}  // namespace

int main() {
    test_apply_identity_passthrough();
    test_apply_lgg_parity();
    test_apply_lgg_offset_parity();
    test_apply_cdl_parity();
    test_apply_curves_parity();
    test_is_identity_kinds();
    test_names();
    test_serialize_delegates();

    if (failures == 0) {
        std::printf("ALL TESTS PASSED\n");
        return 0;
    }
    std::fprintf(stderr, "%d FAILURES\n", failures);
    return 1;
}