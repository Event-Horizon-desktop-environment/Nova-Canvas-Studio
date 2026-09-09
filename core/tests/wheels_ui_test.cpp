// Phase 4 tests for the headless wheel-panel controller (colorsci/wheels_ui).
// Verifies the pagination state machine, the wheel puck->offset + master-slider
// mapping laws, the Log zero-overlap band partition, HDR zone weighting, the
// CDL<->LGG interchange inverse, and a full panel->grade-graph params round-trip
// (the same interaction the GUI panel performs, headlessly). Links only
// canvas_core.

#include "canvas/core/colorsci/wheels_ui.hpp"
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

constexpr float kClose = 1e-4f;

bool near(float a, float b) {
    return std::fabs(a - b) < kClose;
}

bool rgb_near(const cs::RGBF& a, const cs::RGBF& b) {
    return near(a.r, b.r) && near(a.g, b.g) && near(a.b, b.b);
}

bool lgg_near(const cs::LGG& a, const cs::LGG& b) {
    return near(a.lift_master, b.lift_master) && near(a.gamma_master, b.gamma_master) &&
           near(a.gain_master, b.gain_master) && near(a.lift_r, b.lift_r) &&
           near(a.lift_g, b.lift_g) && near(a.lift_b, b.lift_b) &&
           near(a.gamma_r, b.gamma_r) && near(a.gamma_g, b.gamma_g) &&
           near(a.gamma_b, b.gamma_b) && near(a.gain_r, b.gain_r) &&
           near(a.gain_g, b.gain_g) && near(a.gain_b, b.gain_b);
}

void test_pagination() {
    check(cs::wheel_page_next(cs::WheelPage::kPrimaries) == cs::WheelPage::kLog,
          "next: primaries -> log");
    check(cs::wheel_page_next(cs::WheelPage::kLog) == cs::WheelPage::kHdr, "next: log -> hdr");
    check(cs::wheel_page_next(cs::WheelPage::kHdr) == cs::WheelPage::kCdl, "next: hdr -> cdl");
    check(cs::wheel_page_next(cs::WheelPage::kCdl) == cs::WheelPage::kPrimaries,
          "next wraps cdl -> primaries");
    check(cs::wheel_page_prev(cs::WheelPage::kPrimaries) == cs::WheelPage::kCdl,
          "prev wraps primaries -> cdl");
    check(cs::wheel_page_prev(cs::WheelPage::kLog) == cs::WheelPage::kPrimaries,
          "prev: log -> primaries");
    check(cs::clamp_wheel_page(-3) == cs::WheelPage::kPrimaries, "clamp below -> primaries");
    check(cs::clamp_wheel_page(4) == cs::WheelPage::kCdl, "clamp above -> cdl");
    check(cs::clamp_wheel_page(2) == cs::WheelPage::kHdr, "clamp inside keeps the page");
    check(cs::wheel_page_title(cs::WheelPage::kHdr) != nullptr, "page titles are non-null");
}

void test_master_law() {
    // Identity sits exactly at mid; bounds at 0/1.
    check(near(cs::master_to_value(0.5f, -1.0f, 1.0f, 0.0f), 0.0f), "master 0.5 -> mid (lift)");
    check(near(cs::master_to_value(0.0f, -1.0f, 1.0f, 0.0f), -1.0f), "master 0 -> lo");
    check(near(cs::master_to_value(1.0f, -1.0f, 1.0f, 0.0f), 1.0f), "master 1 -> hi");
    check(near(cs::master_to_value(0.5f, 0.25f, 4.0f, 1.0f), 1.0f), "master 0.5 -> gamma identity");
    check(near(cs::master_to_value(0.25f, 0.0f, 2.0f, 1.0f), 0.5f), "master .25 -> gain quarter");

    // Inverse round-trips and clamps out-of-range values.
    check(near(cs::value_to_master(cs::master_to_value(0.3f, -1.0f, 1.0f, 0.0f), -1.0f, 1.0f, 0.0f),
               0.3f),
          "master_to_value then value_to_master round-trips");
    check(near(cs::value_to_master(9.0f, 0.0f, 2.0f, 1.0f), 1.0f), "value_to_master clamps high");
}

void test_scaled_wheel_offset() {
    check(rgb_near(cs::scaled_wheel_offset(0.0f, 0.0f, 1.0f), cs::RGBF{}),
          "scaled wheel offset: center is zero");
    const cs::RGBF east = cs::scaled_wheel_offset(1.0f, 0.0f, 1.0f);
    check(near(east.r, 1.0f) && near(east.g, -0.5f), "full-right puck pulls red, dips green/blue");
    check(near(cs::scaled_wheel_offset(0.5f, 0.0f, 1.0f).r, 0.5f), "radius scales the offset");
    check(near(cs::scaled_wheel_offset(1.0f, 0.0f, 2.0f).r, 2.0f), "scale amplifies the offset");
    check(cs::scaled_wheel_offset(-1.0f, 0.0f, 1.0f).r < 0.0f, "left puck pulls cyan (negative red)");
}

void test_primaries_wheel_apply() {
    cs::WheelPanelState s;

    // Lift wheel: full-right puck, master at neutral -> the LGG lift terms take
    // exactly the puck offsets, master stays 0.
    cs::apply_primaries_wheel(s, cs::PrimariesWheel::kLift, 1.0f, 0.0f, 0.5f);
    check(near(s.lgg.lift_r, 1.0f) && near(s.lgg.lift_g, -0.5f), "lift wheel writes puck offsets");
    check(near(s.lgg.lift_master, 0.0f), "lift wheel keeps master neutral at 0.5");
    check(lgg_near(cs::LGG{}, s.lgg) == false, "lift wheel actually changed the LGG");

    // Master-only: puck at center, slider to 25% -> master midpoint, offsets zeroed.
    cs::apply_primaries_wheel(s, cs::PrimariesWheel::kLift, 0.0f, 0.0f, 0.25f);
    check(near(s.lgg.lift_master, cs::master_to_value(0.25f, cs::kLiftLo, cs::kLiftHi, 0.0f)),
          "lift wheel master slider drives lift_master");
    check(near(s.lgg.lift_r, 0.0f), "center puck zeroes per-channel lift");

    // Gamma wheel: offsets are 1-centered, master is multiplicative.
    cs::apply_primaries_wheel(s, cs::PrimariesWheel::kGamma, 0.5f, 0.0f, 0.5f);
    check(near(s.lgg.gamma_r, 1.0f + 1.0f), "gamma wheel puts puck offset above 1");
    check(near(s.lgg.gamma_master, 1.0f), "gamma wheel master neutral at 0.5");

    // Gain wheel composed with the master: gain_* = 1 + offset.
    cs::apply_primaries_wheel(s, cs::PrimariesWheel::kGain, 1.0f, 0.0f, 0.5f);
    check(near(s.lgg.gain_r, 2.0f) && near(s.lgg.gain_b, 0.5f), "gain wheel writes 1+offsets");

    // Offset wheel: flat additive channel terms + master.
    cs::apply_primaries_wheel(s, cs::PrimariesWheel::kOffset, 1.0f, 0.0f, 0.5f);
    check(near(s.offset.r, 1.0f) && near(s.offset.master, 0.0f), "offset wheel writes additive terms");

    // Out-of-range puck positions clamp into the shared ranges (never NaN/illegal).
    cs::apply_primaries_wheel(s, cs::PrimariesWheel::kLift, 9.0f, 9.0f, 2.0f);
    check(s.lgg.lift_r >= cs::kLiftLo && s.lgg.lift_r <= cs::kLiftHi, "lift puck clamps into range");
    check(s.lgg.lift_master >= cs::kLiftLo && s.lgg.lift_master <= cs::kLiftHi,
          "lift master clamps into range");

    // Reset: wheel terms return to identity, other wheels untouched.
    const float lift_master_before = s.lgg.lift_master;
    const float lift_r_before = s.lgg.lift_r;
    cs::apply_primaries_wheel(s, cs::PrimariesWheel::kGain, 1.0f, 0.0f, 0.75f);
    cs::reset_primaries_wheel(s, cs::PrimariesWheel::kGain);
    check(near(s.lgg.gain_master, 1.0f) && near(s.lgg.gain_g, 1.0f), "gain wheel resets to 1");
    check(near(s.lgg.lift_master, lift_master_before) && near(s.lgg.lift_r, lift_r_before),
          "gain reset leaves the lift wheel alone");

    // Full reset -> fresh identity panel.
    cs::apply_primaries_wheel(s, cs::PrimariesWheel::kLift, 1.0f, 0.0f, 0.5f);
    cs::reset_panel(s);
    check(lgg_near(s.lgg, cs::LGG{}) && near(s.offset.master, 0.0f), "reset_panel is identity");
}

void test_log_bands() {
    std::array<cs::WheelRange, 4> bands;
    bands[0] = {0.4f, 0.6f};  // shadow
    bands[1] = {0.1f, 0.2f};  // midtone (overlaps shadow, out of order)
    bands[2] = {0.8f, 0.9f};  // highlight
    bands[3] = {0.5f, 1.2f};  // offset (unsorted, hi out of range)
    cs::enforce_zero_overlap(bands);

    float running = 0.0f;
    bool zero_overlap = true;
    for (const auto& b : bands) {
        if (b.lo < running - 1e-4f) zero_overlap = false;
        if (b.hi < b.lo) zero_overlap = false;
        running = b.hi;
    }
    check(zero_overlap, "enforce_zero_overlap tiles the four bands with no overlap");
    check(near(bands.back().hi, 1.0f), "last band reaches 1.0");
    check(near(bands.front().lo, 0.0f), "first band starts at 0.0");

    // Band weight: 1 deep inside, easing at edges, 0 outside; uniform = 1 everywhere.
    cs::WheelRange band{0.2f, 0.4f};
    check(near(cs::log_band_weight(0.3f, band, false), 1.0f), "band weight 1 at band center");
    check(near(cs::log_band_weight(0.1f, band, false), 0.0f), "band weight 0 before the band");
    check(near(cs::log_band_weight(0.5f, band, false), 0.0f), "band weight 0 after the band");
    const float edge = cs::log_band_weight(0.22f, band, false);
    check(edge > 0.0f && edge < 1.0f, "band weight eases at the low edge");
    check(near(cs::log_band_weight(0.3f, band, true), 1.0f), "uniform band is 1 everywhere");
    check(near(cs::log_band_weight(0.0f, band, true), 1.0f), "uniform band 1 out of range too");
}

void test_hdr_zones() {
    cs::HdrZone global;
    global.uniform = true;
    check(near(cs::hdr_zone_weight(0.0f, global), 1.0f) && near(cs::hdr_zone_weight(1.0f, global), 1.0f),
          "hdr global zone weighs 1.0 everywhere");

    cs::HdrZone spot;
    spot.position = 0.5f;
    spot.falloff = 0.2f;
    check(near(cs::hdr_zone_weight(0.5f, spot), 1.0f), "hdr zone weight 1 at its center");
    check(near(cs::hdr_zone_weight(0.5f + 0.2f, spot), 0.0f), "hdr zone weight 0 at falloff edge");
    const float mid = cs::hdr_zone_weight(0.6f, spot);
    check(mid > 0.0f && mid < 1.0f, "hdr zone weight eases within the falloff");
    check(cs::hdr_zone_weight(0.15f, spot) >= 0.0f, "hdr zone weight is non-negative outside");

    cs::HdrZone hard;
    hard.position = 0.0f;
    check(near(cs::hdr_zone_weight(0.0f, hard), 1.0f) && near(cs::hdr_zone_weight(0.5f, hard), 0.0f),
          "hdr zero-falloff zone is a hard point");
}

void test_cdl_interchange() {
    cs::Cdl c;
    c.slope_r = 1.4f;
    c.slope_g = 1.1f;
    c.slope_b = 0.8f;
    c.power_r = 0.9f;
    c.power_g = 1.0f;
    c.power_b = 1.2f;
    c.sat = 1.0f;

    // CDL -> LGG -> cdl_from_lgg must be an exact round-trip for offset-free rows.
    const cs::LGG l = cs::lgg_from_cdl(c);
    check(near(l.gain_r, 1.4f) && near(l.gain_g, 1.1f) && near(l.gain_b, 0.8f),
          "lgg_from_cdl maps slope to gain");
    check(near(l.gamma_r, 1.0f / 0.9f) && near(l.gamma_b, 1.0f / 1.2f),
          "lgg_from_cdl maps 1/power to gamma");
    check(near(l.lift_master, 0.0f), "lgg_from_cdl zeroes lift");

    const cs::CdlConversion back = cs::cdl_from_lgg(l);
    check(back.exact, "cdl_from_lgg(lgg_from_cdl) is exact (offset-free row)");
    check(near(back.cdl.slope_r, c.slope_r) && near(back.cdl.slope_g, c.slope_g) &&
              near(back.cdl.slope_b, c.slope_b),
          "slope round-trips CDL -> LGG -> CDL");
    check(near(back.cdl.power_r, c.power_r) && near(back.cdl.power_g, c.power_g) &&
              near(back.cdl.power_b, c.power_b),
          "power round-trips CDL -> LGG -> CDL");
}

void test_panel_to_graph_roundtrip() {
    // Simulate panel interaction: a primaries lift gain on the Lift wheel, a
    // gamma reshape, and a gain lift, then commit the LGG into a grade graph
    // exactly like the GUI panel's set_clip_grade path would.
    cs::WheelPanelState s;
    cs::apply_primaries_wheel(s, cs::PrimariesWheel::kLift, 0.6f, 0.1f, 0.5f);
    cs::apply_primaries_wheel(s, cs::PrimariesWheel::kGamma, 0.2f, -0.4f, 0.6f);
    cs::apply_primaries_wheel(s, cs::PrimariesWheel::kGain, -0.3f, 0.2f, 0.4f);

    gg::GradeGraph g;
    const int c = g.add_node(gg::NodeKind::kCorrector);
    g.node(c).correct_mode = gg::CorrectMode::kLgg;
    g.node(c).lgg = s.lgg;
    const int out = g.add_node(gg::NodeKind::kOutput);
    g.add_rgb_edge(c, out);

    const nlohmann::json j = gg::grade_graph_to_json(g);
    const gg::GradeGraph reloaded = gg::grade_graph_from_json(j);
    check(reloaded.num_nodes() == 2, "grade graph with panel LGG survives JSON");
    const int rc = [&] {
        for (int n = 0; n < static_cast<int>(reloaded.num_nodes()); ++n) {
            if (reloaded.node(n).kind == gg::NodeKind::kCorrector) return n;
        }
        return -1;
    }();
    check(rc >= 0 && reloaded.node(rc).lgg.has_value(), "reloaded graph keeps the corrector LGG");
    if (rc >= 0 && reloaded.node(rc).lgg) {
        check(lgg_near(*reloaded.node(rc).lgg, s.lgg), "panel wheels round-trip through the graph");
    }
}

}  // namespace

int main() {
    test_pagination();
    test_master_law();
    test_scaled_wheel_offset();
    test_primaries_wheel_apply();
    test_log_bands();
    test_hdr_zones();
    test_cdl_interchange();
    test_panel_to_graph_roundtrip();

    if (failures == 0) {
        std::printf("ALL TESTS PASSED\n");
        return 0;
    }
    std::fprintf(stderr, "%d FAILURES\n", failures);
    return 1;
}