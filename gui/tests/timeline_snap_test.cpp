#include "Widgets/timeline_snap.hpp"

#include <array>
#include <cstdio>
#include <cstdlib>

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

void test_grid_step_zoom_progression() {
    CHECK(timeline_snap::grid_step(0.25) == 1);
    CHECK(timeline_snap::grid_step(0.5) == 2);
    CHECK(timeline_snap::grid_step(1.0) == 4);
    CHECK(timeline_snap::grid_step(2.0) == 8);
    CHECK(timeline_snap::grid_step(4.0) == 16);
    CHECK(timeline_snap::grid_step(30.0) == 128);
    CHECK(timeline_snap::grid_step(0.0) == 1);
    CHECK(timeline_snap::grid_step(-3.0) == 1);
}

void test_snap_to_exact_grid() {
    CHECK(timeline_snap::snap_to_grid(0, 4.0) == 0);
    CHECK(timeline_snap::snap_to_grid(16, 4.0) == 16);
    CHECK(timeline_snap::snap_to_grid(64, 4.0) == 64);
    CHECK(timeline_snap::snap_to_grid(8, 2.0) == 8);
}

void test_snap_rounds_to_nearest() {
    CHECK(timeline_snap::snap_to_grid(33, 4.0) == 32);
    CHECK(timeline_snap::snap_to_grid(47, 4.0) == 48);
    CHECK(timeline_snap::snap_to_grid(13, 2.0) == 16);
    CHECK(timeline_snap::snap_to_grid(20, 2.0) == 24);
    CHECK(timeline_snap::snap_to_grid(5, 1.0) == 4);
    CHECK(timeline_snap::snap_to_grid(7, 1.0) == 8);
    CHECK(timeline_snap::snap_to_grid(12, 2.0) == 16);
    CHECK(timeline_snap::snap_to_grid(40, 4.0) == 48);
    CHECK(timeline_snap::snap_to_grid(6, 1.0) == 8);
}

void test_snap_degenerate_zoom_passthrough() {
    CHECK(timeline_snap::snap_to_grid(50, 0.0) == 50);
    CHECK(timeline_snap::snap_to_grid(50, -1.0) == 50);
}

void test_snap_error_within_half_step() {
    const double fpps[] = {0.25, 0.5, 1.0, 2.0, 4.0, 30.0};
    for (const double fpp : fpps) {
        const int64_t step = timeline_snap::grid_step(fpp);
        for (int64_t f = 0; f <= 400; ++f) {
            const int64_t got = timeline_snap::snap_to_grid(f, fpp);
            const int64_t err = got > f ? got - f : f - got;
            CHECK(err <= step / 2 + step % 2);
            CHECK(got % step == 0);
        }
    }
}

void test_edges_stay_in_place_on_exact_target() {
    const std::array<int64_t, 2> targets{100, 200};
    CHECK(timeline_snap::snap_frame_to_edges(100, 10, targets) == 100);
    CHECK(timeline_snap::snap_frame_to_edges(200, 10, targets) == 200);
}

void test_edges_magnet_within_radius() {
    const std::array<int64_t, 2> targets{100, 200};
    CHECK(timeline_snap::snap_frame_to_edges(93, 10, targets) == 100);
    CHECK(timeline_snap::snap_frame_to_edges(108, 10, targets) == 100);
    CHECK(timeline_snap::snap_frame_to_edges(104, 4, targets) == 100);
    CHECK(timeline_snap::snap_frame_to_edges(196, 4, targets) == 200);
}

void test_edges_inert_beyond_radius() {
    const std::array<int64_t, 2> targets{100, 200};
    CHECK(timeline_snap::snap_frame_to_edges(88, 10, targets) == 88);
    CHECK(timeline_snap::snap_frame_to_edges(113, 10, targets) == 113);
    CHECK(timeline_snap::snap_frame_to_edges(90, 10, targets) == 100);
    CHECK(timeline_snap::snap_frame_to_edges(89, 10, targets) == 89);
}

void test_edges_empty_and_degenerate() {
    const std::array<int64_t, 0> empty{};
    CHECK(timeline_snap::snap_frame_to_edges(50, 10, empty) == 50);
    CHECK(timeline_snap::snap_frame_to_edges(50, 0, std::array<int64_t, 1>{48}) == 50);
    CHECK(timeline_snap::snap_frame_to_edges(50, -1, std::array<int64_t, 1>{48}) == 50);
}

void test_edges_scope_to_nearest_target() {
    CHECK(timeline_snap::snap_frame_to_edges(96, 10, std::array<int64_t, 2>{92, 100}) == 100);
    CHECK(timeline_snap::snap_frame_to_edges(196, 10, std::array<int64_t, 2>{100, 200}) == 200);
    CHECK(timeline_snap::snap_frame_to_edges(150, 5, std::array<int64_t, 2>{148, 152}) == 152);
}

void test_dragged_edges_leading_wins() {
    const std::array<int64_t, 2> targets{60, 220};
    CHECK(timeline_snap::snap_dragged_edges(72, 50, targets, 2.0) == 60);
}

void test_dragged_edges_trailing_wins() {
    const std::array<int64_t, 3> targets{40, 100, 180};
    CHECK(timeline_snap::snap_dragged_edges(130, 50, targets, 0.5) == 130);
    CHECK(timeline_snap::snap_dragged_edges(135, 50, targets, 0.5) == 130);
}

void test_dragged_edges_tie_and_closer_pull() {
    const std::array<int64_t, 3> targets{79, 100, 130};
    CHECK(timeline_snap::snap_dragged_edges(80, 50, targets, 0.5) == 79);
    CHECK(timeline_snap::snap_dragged_edges(81, 51, targets, 0.5) == 79);
    const std::array<int64_t, 2> t2{100, 150};
    CHECK(timeline_snap::snap_dragged_edges(98, 51, t2, 0.5) == 99);
}

void test_dragged_edges_clamp_at_zero() {
    CHECK(timeline_snap::snap_dragged_edges(5, 14, std::array<int64_t, 1>{13}, 1.0) == 0);
    CHECK(timeline_snap::snap_dragged_edges(5, 3, std::array<int64_t, 1>{100}, 0.5) == 5);
    CHECK(timeline_snap::snap_dragged_edges(5, 3, std::array<int64_t, 0>{}, 0.5) == 5);
}

void test_dragged_edges_inert_and_degenerate() {
    const std::array<int64_t, 2> targets{10, 500};
    CHECK(timeline_snap::snap_dragged_edges(200, 50, targets, 0.5) == 200);
    CHECK(timeline_snap::snap_dragged_edges(72, 50, targets, 0.0) == 72);
    CHECK(timeline_snap::snap_dragged_edges(72, 50, targets, -1.0) == 72);
    CHECK(timeline_snap::snap_dragged_edges(72, 50, std::array<int64_t, 0>{}, 0.5) == 72);
    CHECK(timeline_snap::snap_dragged_edges(72, 0, targets, 0.5) == 72);
}

}

int main() {
    test_grid_step_zoom_progression();
    test_snap_to_exact_grid();
    test_snap_rounds_to_nearest();
    test_snap_degenerate_zoom_passthrough();
    test_snap_error_within_half_step();
    test_edges_stay_in_place_on_exact_target();
    test_edges_magnet_within_radius();
    test_edges_inert_beyond_radius();
    test_edges_empty_and_degenerate();
    test_edges_scope_to_nearest_target();
    test_dragged_edges_leading_wins();
    test_dragged_edges_trailing_wins();
    test_dragged_edges_tie_and_closer_pull();
    test_dragged_edges_clamp_at_zero();
    test_dragged_edges_inert_and_degenerate();

    if (g_failures == 0) {
        std::printf("timeline_snap_test: ALL PASS\n");
        return 0;
    }
    std::printf("timeline_snap_test: %d FAILURE(S)\n", g_failures);
    return 1;
}