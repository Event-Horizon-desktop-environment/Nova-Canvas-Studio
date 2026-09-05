// Headless TimelineSnap unit tests (Phase 27). Compiles timeline_snap.cpp
// directly into the binary so the module is verified exactly as shipped; the
// Qt-free seam is enforced by the build (a stray <Q...> include breaks this
// target on purpose).
//
// The snap grid is zoom-dependent (visual quantization to ~24px): the step is
// the largest power-of-two frame count whose on-screen width stays under 24px at
// the given frames-per-pixel zoom. These tests pin that math (including the
// llround half-away-from-zero rounding at grid boundaries) so refactors cannot
// silently change scrubbing/drag feel.

#include "Widgets/timeline_snap.hpp"

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
    // Coarser zoom (fewer pixels per frame) -> larger step.
    CHECK(timeline_snap::grid_step(0.25) == 1);
    CHECK(timeline_snap::grid_step(0.5) == 2);
    CHECK(timeline_snap::grid_step(1.0) == 4);
    CHECK(timeline_snap::grid_step(2.0) == 8);
    CHECK(timeline_snap::grid_step(4.0) == 16);
    // Way zoomed out: the step keeps doubling until its 10x width clears 24px.
    CHECK(timeline_snap::grid_step(30.0) == 128);
    // Non-positive / degenerate zoom degrades to frame-granular, never div-by-zero.
    CHECK(timeline_snap::grid_step(0.0) == 1);
    CHECK(timeline_snap::grid_step(-3.0) == 1);
}

void test_snap_to_exact_grid() {
    // On-grid frames snap to themselves.
    CHECK(timeline_snap::snap_to_grid(0, 4.0) == 0);
    CHECK(timeline_snap::snap_to_grid(16, 4.0) == 16);
    CHECK(timeline_snap::snap_to_grid(64, 4.0) == 64);
    CHECK(timeline_snap::snap_to_grid(8, 2.0) == 8);
}

void test_snap_rounds_to_nearest() {
    // Nearest multiple wins (plain rounding).
    CHECK(timeline_snap::snap_to_grid(33, 4.0) == 32);   // 33/16 = 2.06 -> 32
    CHECK(timeline_snap::snap_to_grid(47, 4.0) == 48);   // 47/16 = 2.94 -> 48
    CHECK(timeline_snap::snap_to_grid(13, 2.0) == 16);   // 13/8  = 1.63 -> 16
    CHECK(timeline_snap::snap_to_grid(20, 2.0) == 24);   // 20/8  = 2.50 -> 24
    CHECK(timeline_snap::snap_to_grid(5, 1.0) == 4);     // 5/4   = 1.25 -> 4
    CHECK(timeline_snap::snap_to_grid(7, 1.0) == 8);     // 7/4   = 1.75 -> 8
    // Exact half steps round away from zero (llround) — pinned behavior.
    CHECK(timeline_snap::snap_to_grid(12, 2.0) == 16);   // 12/8  = 1.50 -> 16
    CHECK(timeline_snap::snap_to_grid(40, 4.0) == 48);   // 40/16 = 2.50 -> 48
    CHECK(timeline_snap::snap_to_grid(6, 1.0) == 8);     // 6/4   = 1.50 -> 8
}

void test_snap_degenerate_zoom_passthrough() {
    // Defensive: non-positive zoom passes the frame through unchanged.
    CHECK(timeline_snap::snap_to_grid(50, 0.0) == 50);
    CHECK(timeline_snap::snap_to_grid(50, -1.0) == 50);
}

void test_snap_error_within_half_step() {
    // Invariant: |snap(f) - f| <= step/2 for every snap in normal use.
    const double fpps[] = {0.25, 0.5, 1.0, 2.0, 4.0, 30.0};
    for (const double fpp : fpps) {
        const int64_t step = timeline_snap::grid_step(fpp);
        for (int64_t f = 0; f <= 400; ++f) {
            const int64_t got = timeline_snap::snap_to_grid(f, fpp);
            const int64_t err = got > f ? got - f : f - got;
            CHECK(err <= step / 2 + step % 2);
            // Result must be a multiple of the step.
            CHECK(got % step == 0);
        }
    }
}

}  // namespace

int main() {
    test_grid_step_zoom_progression();
    test_snap_to_exact_grid();
    test_snap_rounds_to_nearest();
    test_snap_degenerate_zoom_passthrough();
    test_snap_error_within_half_step();

    if (g_failures == 0) {
        std::printf("timeline_snap_test: ALL PASS\n");
        return 0;
    }
    std::printf("timeline_snap_test: %d FAILURE(S)\n", g_failures);
    return 1;
}