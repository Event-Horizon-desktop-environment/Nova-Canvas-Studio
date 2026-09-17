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

constexpr double kBox = 40.0;

void test_zero_db_is_center() {
    CHECK(std::abs(timeline_volume_line::volume_line_y(0.0, kBox) - kBox / 2.0) < 1e-9);
}

void test_band_edges_pin_top_and_bottom() {
    const double top = timeline_volume_line::volume_line_y(24.0, kBox);
    const double bottom = timeline_volume_line::volume_line_y(-100.0, kBox);
    CHECK(std::abs(top - timeline_volume_line::kVolumeLinePad) < 1e-9);
    CHECK(std::abs(bottom - (kBox - timeline_volume_line::kVolumeLinePad)) < 1e-9);
    const double zero = timeline_volume_line::volume_line_y(0.0, kBox);
    CHECK(zero > top);
    CHECK(zero < bottom);
}

void test_invert_preserves_band() {
    for (double db : {-100.0, -80.0, -60.0, -40.0, -20.0, -6.0, 0.0, 6.0, 12.0, 24.0}) {
        const double y = timeline_volume_line::volume_line_y(db, kBox);
        const double back = timeline_volume_line::db_from_volume_line_y(y, kBox);
        CHECK(std::abs(back - db) < 0.5);
    }
    CHECK(std::abs(timeline_volume_line::volume_line_y(-100.0, kBox) -
                   timeline_volume_line::volume_line_y(-24.0, kBox)) > 1.0);
}

void test_upper_is_on_top_lower_on_bottom() {
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
    for (double y : {-100.0, -1.0, 0.0, kBox - 1.0, kBox + 5.0, 1000.0}) {
        const double db = timeline_volume_line::db_from_volume_line_y(y, kBox);
        CHECK(db >= -100.0);
        CHECK(db <= 24.0);
    }
    const double floor_db = timeline_volume_line::db_from_volume_line_y(kBox * 2.0, kBox);
    CHECK(std::abs(floor_db - (-100.0)) < 1e-9);
}

void test_monotonic() {
    double prev_y = timeline_volume_line::volume_line_y(-100.0, kBox);
    for (double db = -100.0; db <= 24.0; db += 1.0) {
        const double y = timeline_volume_line::volume_line_y(db, kBox);
        CHECK(y <= prev_y + 1e-9);
        prev_y = y;
    }
}

void test_up_from_zero_is_louder() {
    const double center = timeline_volume_line::volume_line_y(0.0, kBox);
    const double above_db = timeline_volume_line::db_from_volume_line_y(center - 4.0, kBox);
    const double below_db = timeline_volume_line::db_from_volume_line_y(center + 4.0, kBox);
    CHECK(above_db > 0.0);
    CHECK(below_db < 0.0);
    CHECK(above_db > below_db);
}

void test_drag_curve_fine_grains() {
    const double center = timeline_volume_line::volume_line_y(0.0, kBox);
    const double dy = 12.0;
    const double linear = timeline_volume_line::drag_db_from_relative_y(center - dy, kBox, 1.0);
    const double curved = timeline_volume_line::drag_db_from_relative_y(
        center - dy, kBox, timeline_volume_line::kVolumeLineDragExponent);
    CHECK(linear > 0.0);
    CHECK(curved > 0.0);
    CHECK(curved < linear);
    CHECK(curved > linear / (timeline_volume_line::kVolumeLineDragExponent + 0.5));
    const double up = timeline_volume_line::drag_db_from_relative_y(
        center - 6.0, kBox, timeline_volume_line::kVolumeLineDragExponent);
    const double down = timeline_volume_line::drag_db_from_relative_y(
        center + 6.0, kBox, timeline_volume_line::kVolumeLineDragExponent);
    CHECK(up > 0.0);
    CHECK(down < 0.0);
    const double edge_top = timeline_volume_line::drag_db_from_relative_y(
        0.0, kBox, timeline_volume_line::kVolumeLineDragExponent);
    const double edge_bottom = timeline_volume_line::drag_db_from_relative_y(
        kBox, kBox, timeline_volume_line::kVolumeLineDragExponent);
    CHECK(std::abs(edge_top - 24.0) < 1e-9);
    CHECK(std::abs(edge_bottom - (-100.0)) < 1e-9);
    const double far_top = timeline_volume_line::drag_db_from_relative_y(
        -kBox * 3.0, kBox, timeline_volume_line::kVolumeLineDragExponent);
    const double far_bottom = timeline_volume_line::drag_db_from_relative_y(
        kBox * 3.0, kBox, timeline_volume_line::kVolumeLineDragExponent);
    CHECK(std::abs(far_top - 24.0) < 1e-9);
    CHECK(std::abs(far_bottom - (-100.0)) < 1e-9);
}

void test_waveform_scale_never_flattens() {
    const double full = timeline_volume_line::volume_waveform_scale(0.0);
    const double loud = timeline_volume_line::volume_waveform_scale(24.0);
    const double quiet = timeline_volume_line::volume_waveform_scale(-100.0);
    const double mid = timeline_volume_line::volume_waveform_scale(-12.0);
    const double deep = timeline_volume_line::volume_waveform_scale(-60.0);
    CHECK(std::abs(full - 1.0) < 1e-9);
    CHECK(std::abs(loud - 1.0) < 1e-9);
    CHECK(quiet > 0.0);
    CHECK(quiet >= timeline_volume_line::kVolumeWaveformScaleFloor);
    CHECK(quiet < deep);
    CHECK(deep < mid);
    CHECK(mid < full);
}

void test_degenerate_body_height() {
    const double y = timeline_volume_line::volume_line_y(0.0, 1.0);
    CHECK(y >= 0.0);
    CHECK(y <= 1.0);
    const double flat = timeline_volume_line::volume_waveform_scale(-120.0);
    CHECK(flat >= timeline_volume_line::kVolumeWaveformScaleFloor);
    CHECK(flat <= 1.0);
}

}

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
