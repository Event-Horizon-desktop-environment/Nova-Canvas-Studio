// Headless timeline_volume_line unit tests (Phase 5). Compiles
// timeline_volume_line.cpp directly into the binary so the module is verified
// exactly as shipped; the Qt-free seam is enforced by the build (a stray
// <Q...> include breaks this target on purpose).
//
// The volume line maps the audio clip's volume_db to a vertical position inside
// the waveform box: 0 dB sits dead-center, +24 dB rides the top edge and -100 dB
// (silence, db_to_gain(-100) = 0) rides the bottom edge. Dragging UP from the
// center is therefore ALWAYS louder and dragging DOWN always quieter, and the
// very bottom of a clip is digital silence (no invisible visual floor as under
// the old gutted band). These tests pin the law's shape (including the pad
// floor, the 0-dB center, and the clamping into the shared audio_mix band) so
// refactors cannot silently flip the drag direction or let a position escape
// the visible box.

#include "Widgets/timeline_volume_line.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <initializer_list>

using namespace canvas::gui;

namespace {

int g_failures = 0;

#define CHECK(cond)                                                         \
    do {                                                                    \
        if (!(cond)) {                                                      \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);     \
            ++g_failures;                                                   \
        }                                                                   \
    } while (0)

constexpr double kBox = 40.0;  // typical audio-clip waveform box height (px)

void test_zero_db_is_center() {
    // 0 dB maps to dead center, so "drag up = louder, drag down = quieter" is
    // unambiguous for any normal box.
    CHECK(std::abs(timeline_volume_line::volume_line_y(0.0, kBox) - kBox / 2.0) < 1e-9);
}

void test_band_edges_pin_top_and_bottom() {
    // +24 dB rides the top edge, -100 dB (silence) the bottom edge.
    const double top = timeline_volume_line::volume_line_y(24.0, kBox);
    const double bottom = timeline_volume_line::volume_line_y(-100.0, kBox);
    CHECK(std::abs(top - timeline_volume_line::kVolumeLinePad) < 1e-9);
    CHECK(std::abs(bottom - (kBox - timeline_volume_line::kVolumeLinePad)) < 1e-9);
    // A live 0-dB line is between them, closer to the top (24 dB up-travel vs
    // 100 dB down-travel) but strictly above the -100 bottom.
    const double zero = timeline_volume_line::volume_line_y(0.0, kBox);
    CHECK(zero > top);
    CHECK(zero < bottom);
}

void test_invert_preserves_band() {
    // Round-trip through the FULL audio law band (the whole band is on screen).
    for (double db : {-100.0, -80.0, -60.0, -40.0, -20.0, -6.0, 0.0, 6.0, 12.0, 24.0}) {
        const double y = timeline_volume_line::volume_line_y(db, kBox);
        const double back = timeline_volume_line::db_from_volume_line_y(y, kBox);
        CHECK(std::abs(back - db) < 0.5);
    }
    // -100 dB and -24 dB are positionally DISTINCT (bottom edge vs inside): no
    // invisible visual floor, muting is reachable by pulling to the bottom.
    CHECK(std::abs(timeline_volume_line::volume_line_y(-100.0, kBox) -
                   timeline_volume_line::volume_line_y(-24.0, kBox)) > 1.0);
}

void test_upper_is_on_top_lower_on_bottom() {
    // +24 dB rides near the top edge; -100 dB near the bottom (drag DOWN = quieter).
    const double top = timeline_volume_line::volume_line_y(24.0, kBox);
    const double bottom = timeline_volume_line::volume_line_y(-100.0, kBox);
    CHECK(top < kBox / 2.0);
    CHECK(bottom > kBox / 2.0);
    CHECK(std::abs(top - timeline_volume_line::kVolumeLinePad) < 1e-9);
    CHECK(std::abs(bottom - (kBox - timeline_volume_line::kVolumeLinePad)) < 1e-9);
}

void test_positions_never_leave_the_visible_box() {
    for (double db = -120.0; db <= 60.0; db += 3.0) {
        const double y = timeline_volume_line::volume_line_y(db, kBox);
        CHECK(y >= timeline_volume_line::kVolumeLinePad);
        CHECK(y <= kBox - timeline_volume_line::kVolumeLinePad);
    }
    // Out-of-range positions clamp into the band too.
    for (double y : {-100.0, -1.0, 0.0, kBox - 1.0, kBox + 5.0, 1000.0}) {
        const double db = timeline_volume_line::db_from_volume_line_y(y, kBox);
        CHECK(db >= -100.0);
        CHECK(db <= 24.0);
    }
    // The clamp must never hand back anything quieter than the law floor, so a
    // line dragged to the very bottom of the clip is exactly silence (-100 dB),
    // which db_to_gain maps to 0.0 gain.
    const double floor_db = timeline_volume_line::db_from_volume_line_y(kBox * 2.0, kBox);
    CHECK(std::abs(floor_db - (-100.0)) < 1e-9);
}

void test_monotonic() {
    // Higher position (smaller y) must never mean lower volume: dragging up is
    // always LOUDER, dragging down always QUIETER, across the whole band.
    double prev_y = timeline_volume_line::volume_line_y(-100.0, kBox);
    for (double db = -100.0; db <= 24.0; db += 1.0) {
        const double y = timeline_volume_line::volume_line_y(db, kBox);
        CHECK(y <= prev_y + 1e-9);
        prev_y = y;
    }
}

void test_up_from_zero_is_louder() {
    // The direction the user cares about, pinned without mouse simulation: a
    // position just ABOVE the 0-dB center (smaller y) round-trips LOUDER, one
    // just BELOW it QUIETER.
    const double center = timeline_volume_line::volume_line_y(0.0, kBox);
    const double above_db = timeline_volume_line::db_from_volume_line_y(center - 4.0, kBox);
    const double below_db = timeline_volume_line::db_from_volume_line_y(center + 4.0, kBox);
    CHECK(above_db > 0.0);
    CHECK(below_db < 0.0);
    CHECK(above_db > below_db);
}

void test_drag_curve_fine_grains() {
    // With the drag curve exponent >1 the same physical travel near the center
    // swings a SMALLER dB value than the linear map (curve 1): finer-grained,
    // slower, less jump. 12px up from center at curve 1.5 must be gentler than
    // the linear swing, and both must be positive (louder).
    const double center = timeline_volume_line::volume_line_y(0.0, kBox);
    const double dy = 12.0;
    const double linear = timeline_volume_line::drag_db_from_relative_y(center - dy, kBox, 1.0);
    const double curved = timeline_volume_line::drag_db_from_relative_y(
        center - dy, kBox, timeline_volume_line::kVolumeLineDragExponent);
    CHECK(linear > 0.0);
    CHECK(curved > 0.0);
    CHECK(curved < linear);
    CHECK(curved > linear / (timeline_volume_line::kVolumeLineDragExponent + 0.5));
    // Direction holds at any curve: up = louder, down = quieter.
    const double up = timeline_volume_line::drag_db_from_relative_y(
        center - 6.0, kBox, timeline_volume_line::kVolumeLineDragExponent);
    const double down = timeline_volume_line::drag_db_from_relative_y(
        center + 6.0, kBox, timeline_volume_line::kVolumeLineDragExponent);
    CHECK(up > 0.0);
    CHECK(down < 0.0);
    // Fine grain must never cost the extremes: the box edges themselves map to
    // the band limits EXACTLY (|1|^curve == 1), so dragging the line to the very
    // top/bottom of the clip reaches +24 dB / -100 dB — no extra travel needed.
    const double edge_top = timeline_volume_line::drag_db_from_relative_y(
        0.0, kBox, timeline_volume_line::kVolumeLineDragExponent);
    const double edge_bottom = timeline_volume_line::drag_db_from_relative_y(
        kBox, kBox, timeline_volume_line::kVolumeLineDragExponent);
    CHECK(std::abs(edge_top - 24.0) < 1e-9);
    CHECK(std::abs(edge_bottom - (-100.0)) < 1e-9);
    // Gestures beyond the box clamp to the band limits too.
    const double far_top = timeline_volume_line::drag_db_from_relative_y(
        -kBox * 3.0, kBox, timeline_volume_line::kVolumeLineDragExponent);
    const double far_bottom = timeline_volume_line::drag_db_from_relative_y(
        kBox * 3.0, kBox, timeline_volume_line::kVolumeLineDragExponent);
    CHECK(std::abs(far_top - 24.0) < 1e-9);
    CHECK(std::abs(far_bottom - (-100.0)) < 1e-9);
}

void test_waveform_scale_never_flattens() {
    // The spectrum's volume response: full height at 0 dB and louder, shrinking
    // toward the floored minimum as the clip quiets — but NEVER zero, so it can
    // never collapse into a flat line (the old linear gain multiplied bars by
    // db_to_gain -> 0 at the silence floor, which flattened them).
    const double full = timeline_volume_line::volume_waveform_scale(0.0);
    const double loud = timeline_volume_line::volume_waveform_scale(24.0);
    const double quiet = timeline_volume_line::volume_waveform_scale(-100.0);
    const double mid = timeline_volume_line::volume_waveform_scale(-12.0);
    const double deep = timeline_volume_line::volume_waveform_scale(-60.0);
    CHECK(std::abs(full - 1.0) < 1e-9);
    CHECK(std::abs(loud - 1.0) < 1e-9);  // clamped at full height, never over
    CHECK(quiet > 0.0);
    CHECK(quiet >= timeline_volume_line::kVolumeWaveformScaleFloor);
    CHECK(quiet < deep);   // monotone all the way down to -100 dB
    CHECK(deep < mid);
    CHECK(mid < full);  // monotonically louder = fuller
}

void test_degenerate_body_height() {
    // A box shorter than 2*pad can't honor the pad floor: the position must
    // still stay inside the box (no div-by-zero, no overflow out of it).
    const double y = timeline_volume_line::volume_line_y(0.0, 1.0);
    CHECK(y >= 0.0);
    CHECK(y <= 1.0);
    // The waveform scale must be sane for extreme inputs too.
    const double flat = timeline_volume_line::volume_waveform_scale(-120.0);
    CHECK(flat >= timeline_volume_line::kVolumeWaveformScaleFloor);
    CHECK(flat <= 1.0);
}

}  // namespace

int main() {
    test_zero_db_is_center();
    test_band_edges_pin_top_and_bottom();
    test_invert_preserves_band();
    test_upper_is_on_top_lower_on_bottom();
    test_positions_never_leave_the_visible_box();
    test_monotonic();
    test_up_from_zero_is_louder();
    test_drag_curve_fine_grains();
    test_waveform_scale_never_flattens();
    test_degenerate_body_height();
    if (g_failures == 0) {
        std::printf("ALL TESTS PASSED\n");
        return 0;
    }
    std::fprintf(stderr, "%d TEST(S) FAILED\n", g_failures);
    return 1;
}