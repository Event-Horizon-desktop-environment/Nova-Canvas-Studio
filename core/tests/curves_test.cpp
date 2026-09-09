// Phase 5 tests for the custom-curve grade law (colorsci/curves). Verifies
// spline identity/passthrough, exact knot interpolation, regularization of
// duplicate-x points, the folded soft-clip toe/shoulder rails (monotonicity,
// C1 at the rail, 1.0 folding below 1.0), hue-preserving luma curves, and the
// graph-node integration (evaluate + JSON round-trip) for CorrectMode::kCurves.
// Links only canvas_core.

#include "canvas/core/colorsci/curves.hpp"
#include "canvas/core/grade_graph/eval.hpp"
#include "canvas/core/grade_graph/graph.hpp"
#include "canvas/core/grade_graph/serialize.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace cs = canvas::core::colorsci;
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

constexpr float kClose = 1e-3f;

bool near(float a, float b) {
    return std::fabs(a - b) < kClose;
}

bool rgb_near(const cs::RGBF& a, const cs::RGBF& b) {
    return near(a.r, b.r) && near(a.g, b.g) && near(a.b, b.b);
}

cs::RGBF apply_curves_channel_luma(const cs::RGBF& in, const std::vector<cs::CurvePoint>& pts) {
    cs::CurveParams c;
    c.channels[static_cast<std::size_t>(cs::CurveChannel::kLuma)] = pts;
    return cs::apply_curves(in, c);
}

void test_identity() {
    check(cs::CurveParams{}.is_identity(), "empty CurveParams is identity");
    check(cs::SoftClip{}.is_identity(), "default SoftClip is identity");

    cs::CurveParams c;
    const cs::RGBF in{0.33f, 0.55f, 0.11f};
    check(rgb_near(cs::apply_curves(in, c), in), "apply_curves(empty) is passthrough");

    for (float x = 0.0f; x <= 1.0001f; x += 0.1f) {
        if (std::fabs(cs::eval_curve({}, x) - x) > 1e-6f) {
            check(false, "eval_curve(empty) == x across the domain");
            return;
        }
    }
    check(true, "eval_curve(empty) == x across the domain");
}

void test_catmull_rom_knots() {
    // A single interior point off the diagonal: the spline must hit it exactly.
    const std::vector<cs::CurvePoint> pts{{0.5f, 0.8f}};
    check(near(cs::eval_curve(pts, 0.5f), 0.8f), "single point interpolated exactly");
    check(near(cs::eval_curve(pts, 0.0f), 0.0f), "implicit (0,0) endpoint holds");
    check(near(cs::eval_curve(pts, 1.0f), 1.0f), "implicit (1,1) endpoint holds");

    // Two interior points on an S-curve.
    const std::vector<cs::CurvePoint> sc{{0.3f, 0.1f}, {0.7f, 0.9f}};
    check(near(cs::eval_curve(sc, 0.3f), 0.1f), "first S-knot interpolated");
    check(near(cs::eval_curve(sc, 0.7f), 0.9f), "second S-knot interpolated");

    // Out-of-domain x clamps (does not extrapolate past the implicit endpoints).
    check(near(cs::eval_curve(sc, -0.5f), 0.0f), "x < 0 clamps to the start");
    check(near(cs::eval_curve(sc, 1.5f), 1.0f), "x > 1 clamps to the end");
}

void test_sort_and_duplicates() {
    // Points out of x order are sorted.
    const std::vector<cs::CurvePoint> unsorted{{0.8f, 0.2f}, {0.2f, 0.6f}};
    check(near(cs::eval_curve(unsorted, 0.2f), 0.6f), "knots evaluated after x-sort");

    // Duplicate x collapses to the LAST point (drag-on-top wins) and evaluates
    // to a finite value (no zero-length segment NaN).
    const std::vector<cs::CurvePoint> dup{{0.5f, 0.2f}, {0.5f, 0.9f}};
    check(std::isfinite(cs::eval_curve(dup, 0.5f)), "duplicate-x knot stays finite");
    check(near(cs::eval_curve(dup, 0.5f), 0.9f), "duplicate-x keeps the last point");
}

void test_monotone_range() {
    // Monotone control points must yield a monotone, in-range curve.
    const std::vector<cs::CurvePoint> pts{{0.2f, 0.1f}, {0.5f, 0.7f}, {0.8f, 0.9f}};
    float prev = -1.0f;
    bool monotone = true;
    bool in_range = true;
    for (float x = 0.0f; x <= 1.0001f; x += 0.005f) {
        const float y = cs::eval_curve(pts, x);
        if (y < prev - 1e-3f) monotone = false;
        if (y < -1e-3f || y > 1.0001f) in_range = false;
        prev = y;
    }
    check(monotone, "eval_curve is monotone over monotone knots");
    check(in_range, "eval_curve stays within [0,1]");
}

void test_soft_clip_high() {
    // Passthrough when there's nothing to fold.
    check(near(cs::eval_soft_clip_high(0.9f, 1.0f, 1.0f), 0.9f), "high==1 is passthrough");
    check(near(cs::eval_soft_clip_high(0.9f, 0.8f, 0.0f), 0.9f), "soft==0 is passthrough");
    check(near(cs::eval_soft_clip_high(0.75f, 0.8f, 1.0f), 0.75f), "below the rail is passthrough");
    check(near(cs::eval_soft_clip_high(0.8f, 0.8f, 1.0f), 0.8f), "on the rail is unchanged");

    // Monotone, and 1.0 folds strictly below 1.0 for any soft > 0.
    float prev = -1.0f;
    bool monotone = true;
    float top = -1.0f;
    for (float x = 0.7f; x <= 1.0001f; x += 0.01f) {
        const float y = cs::eval_soft_clip_high(x, 0.8f, 0.5f);
        if (y < prev - 1e-3f) monotone = false;
        prev = y;
        top = y;
    }
    check(monotone, "shoulder is monotone");
    check(top < 1.0f - 1e-3f && top > 0.8f, "shoulder folds 1.0 below 1.0");

    // soft == 1 crushes 1.0 onto the rail; C1 continuity at the rail.
    check(near(cs::eval_soft_clip_high(1.0f, 0.8f, 1.0f), 0.8f), "soft==1 crushes the top to the rail");
    const float h = 0.8f;
    const float delta = 1e-3f;
    const float slope = (cs::eval_soft_clip_high(h + delta, h, 0.7f) - h) / delta;
    check(std::fabs(slope - 1.0f) < 5e-2f, "shoulder slope is C1 at the rail");
}

void test_soft_clip_low() {
    check(near(cs::eval_soft_clip_low(0.1f, 0.0f, 1.0f), 0.1f), "low==0 is passthrough");
    check(near(cs::eval_soft_clip_low(0.1f, 0.2f, 0.0f), 0.1f), "low soft==0 is passthrough");
    check(near(cs::eval_soft_clip_low(0.3f, 0.2f, 1.0f), 0.3f), "above the toe is passthrough");
    check(near(cs::eval_soft_clip_low(0.0f, 0.2f, 1.0f), 0.2f), "soft==1 crushes black to the toe");

    float prev = -1.0f;
    bool monotone = true;
    for (float x = 0.0f; x <= 0.3001f; x += 0.005f) {
        const float y = cs::eval_soft_clip_low(x, 0.2f, 0.5f);
        if (y < prev - 1e-3f) monotone = false;
        prev = y;
    }
    check(monotone, "toe is monotone");

    const float lo = 0.2f;
    const float delta = 1e-3f;
    const float slope = (cs::eval_soft_clip_low(lo, lo, 0.7f) - cs::eval_soft_clip_low(lo - delta, lo, 0.7f)) / delta;
    check(std::fabs(slope - 1.0f) < 5e-2f, "toe slope is C1 at the rail");
}

void test_luma_curve_gray() {
    // On gray, the luma curve is applied uniformly (ratio scale == identity on
    // equal channels), so the output is exactly the curve value at that luma.
    const std::vector<cs::CurvePoint> pts{{0.5f, 0.75f}};
    const cs::RGBF out = apply_curves_channel_luma(cs::RGBF{0.4f, 0.4f, 0.4f}, pts);
    const float expected = cs::eval_curve(pts, 0.4f);
    check(rgb_near(out, cs::RGBF{expected, expected, expected}),
          "gray luma curve maps every channel to the curve value");
}

void test_luma_curve_hue_preserving() {
    // On a chromatic pixel the luma curve scales all channels by Lc/L, keeping
    // the RGB ratios (hue) intact: out == in * (Lc / L).
    const std::vector<cs::CurvePoint> pts{{0.5f, 0.6f}};
    const cs::RGBF in{0.5f, 0.25f, 0.125f};
    const float L = cs::kLuma601R * in.r + cs::kLuma601G * in.g + cs::kLuma601B * in.b;
    const float Lc = cs::eval_curve(pts, L);
    const cs::RGBF out = apply_curves_channel_luma(in, pts);
    check(rgb_near(out, cs::RGBF{in.r * Lc / L, in.g * Lc / L, in.b * Lc / L}),
          "luma curve scales RGB by the luma ratio (hue preserved)");
}

void test_channel_curves_isolated() {
    // A red-channel curve only moves red.
    cs::CurveParams c;
    c.channels[static_cast<std::size_t>(cs::CurveChannel::kRed)] = {{0.5f, 0.9f}};
    const cs::RGBF in{0.5f, 0.4f, 0.3f};
    const cs::RGBF out = cs::apply_curves(in, c);
    check(near(out.r, cs::eval_curve({{0.5f, 0.9f}}, 0.5f)), "red curve moves red");
    check(near(out.g, in.g), "green untouched by red curve");
    check(near(out.b, in.b), "blue untouched by red curve");
}

void test_soft_clip_through_apply() {
    // Strong highlight fold via the panel params: a bright pixel drops below a
    // mid pixel's headroom, monotone on a gray ramp, top < input.
    cs::CurveParams c;
    c.soft_clip.high = 0.8f;
    c.soft_clip.high_soft = 0.7f;
    const cs::RGBF bright{0.95f, 0.95f, 0.95f};
    const cs::RGBF out = cs::apply_curves(bright, c);
    check(rgb_near(out, cs::RGBF{out.r, out.r, out.r}), "soft clip preserves gray equality");
    check(out.r < 0.95f - 1e-3f, "highlight fold pulls bright output down");
    check(out.r >= 0.8f - 1e-3f, "highlight fold never drops below the rail");

    cs::CurveParams c2;
    c2.soft_clip.low = 0.2f;
    c2.soft_clip.low_soft = 0.7f;
    const cs::RGBF dark{0.05f, 0.05f, 0.05f};
    const cs::RGBF out2 = cs::apply_curves(dark, c2);
    check(out2.r > 0.05f + 1e-3f, "toe fold pushes black output up");
    check(out2.r <= 0.2f + 1e-3f, "toe fold never exceeds the rail");
}

void test_graph_evaluation_and_roundtrip() {
    // A corrector node carrying a curve for every channel.
    gg::GradeGraph g;
    const int in = g.add_node(gg::NodeKind::kCorrector);
    {
        gg::Node& n = g.node(in);
        n.correct_mode = gg::CorrectMode::kCurves;
        n.curves.emplace();
        n.curves->channels[static_cast<std::size_t>(cs::CurveChannel::kRed)] = {{0.5f, 0.9f}};
        n.curves->channels[static_cast<std::size_t>(cs::CurveChannel::kLuma)] = {{0.5f, 0.6f}};
        n.curves->soft_clip.high = 0.85f;
        n.curves->soft_clip.high_soft = 0.4f;
    }
    const int out = g.add_node(gg::NodeKind::kOutput);
    static_cast<void>(g.add_rgb_edge(in, out));
    const gg::Node& n = g.node(in);  // re-fetch: add_node invalidates held Node&

    gg::FrameF src = gg::FrameF::filled(2, 1, 0.5f, 0.4f, 0.3f, 1.0f);
    const gg::EvalResult res = gg::evaluate_graph(g, src);
    check(!!res.frame, "curves graph evaluates");
    const cs::RGBF expected = cs::apply_curves(cs::RGBF{0.5f, 0.4f, 0.3f}, *n.curves);
    check(near(res.frame->rgba[0], expected.r) && near(res.frame->rgba[4], expected.r),
          "evaluated curves node matches apply_curves");

    // Serialization round-trip.
    const nlohmann::json j = gg::grade_graph_to_json(g);
    const gg::GradeGraph g2 = gg::grade_graph_from_json(j);
    check(g2.num_nodes() == 2, "round-trip keeps the nodes");
    const gg::Node& n2 = g2.node(0);
    check(n2.correct_mode == gg::CorrectMode::kCurves, "round-trip keeps curves mode");
    check(!!n2.curves, "round-trip keeps the curves payload");
    if (n2.curves) {
        check(n2.curves->channels[static_cast<std::size_t>(cs::CurveChannel::kRed)].size() == 1,
              "round-trip keeps red points");
        check(near(n2.curves->channels[static_cast<std::size_t>(cs::CurveChannel::kRed)][0].x, 0.5f),
              "round-trip keeps red x");
        check(near(n2.curves->soft_clip.high, 0.85f), "round-trip keeps soft clip");
    }
}

}  // namespace

int main() {
    test_identity();
    test_catmull_rom_knots();
    test_sort_and_duplicates();
    test_monotone_range();
    test_soft_clip_high();
    test_soft_clip_low();
    test_luma_curve_gray();
    test_luma_curve_hue_preserving();
    test_channel_curves_isolated();
    test_soft_clip_through_apply();
    test_graph_evaluation_and_roundtrip();

    if (failures == 0) {
        std::printf("all checks passed\n");
        return EXIT_SUCCESS;
    }
    std::fprintf(stderr, "%d check(s) FAILED\n", failures);
    return EXIT_FAILURE;
}