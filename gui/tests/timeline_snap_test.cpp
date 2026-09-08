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

// --- Resolve-style magnetic edge snapping (Phase 8) ---------------------------

void test_edges_stay_in_place_on_exact_target() {
    const std::array<int64_t, 2> targets{100, 200};
    // Exactly on a magnet: no pull, identity returned.
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
    // One past the radius from both neighbours: no pull.
    CHECK(timeline_snap::snap_frame_to_edges(88, 10, targets) == 88);
    CHECK(timeline_snap::snap_frame_to_edges(113, 10, targets) == 113);
    // Threshold boundary: exactly at max_delta snaps (<=), just past does not.
    CHECK(timeline_snap::snap_frame_to_edges(90, 10, targets) == 100);
    CHECK(timeline_snap::snap_frame_to_edges(89, 10, targets) == 89);
}

void test_edges_empty_and_degenerate() {
    const std::array<int64_t, 0> empty{};
    CHECK(timeline_snap::snap_frame_to_edges(50, 10, empty) == 50);
    CHECK(timeline_snap::snap_frame_to_edges(50, 0, std::array<int64_t, 1>{48}) == 50);
    // Unsorted targets are a caller contract; an empty span or zero radius are
    // defensively inert rather than UB.
    CHECK(timeline_snap::snap_frame_to_edges(50, -1, std::array<int64_t, 1>{48}) == 50);
}

void test_edges_scope_to_nearest_target() {
    // Left/right targets magnet from the same start: nearest (strictly smaller
    // delta) wins; an exact half-and-half tie keeps the forward (higher) one.
    CHECK(timeline_snap::snap_frame_to_edges(96, 10, std::array<int64_t, 2>{92, 100}) == 100);
    CHECK(timeline_snap::snap_frame_to_edges(196, 10, std::array<int64_t, 2>{100, 200}) == 200);
    CHECK(timeline_snap::snap_frame_to_edges(150, 5, std::array<int64_t, 2>{148, 152}) == 152);
}

void test_dragged_edges_leading_wins() {
    // Leading (raw_tl_in) edge is closest to a target: the clip pulls forward
    // so its head lands exactly on it. Radius = 10px magnet: fpp=2.0 frames/px
    // -> llround(10*2) = 20 frames wide (px*fpp, NOT px/fpp — the on-screen
    // size would otherwise shrink to a fraction at mid zoom-out).
    const std::array<int64_t, 2> targets{60, 220};
    CHECK(timeline_snap::snap_dragged_edges(72, 50, targets, 2.0) == 60);
    // Trailing edge is outside the radius too (72+50=122 vs 220: 98 frames
    // away, inert); head snaps to 60 regardless.
}

void test_dragged_edges_trailing_wins() {
    // Trailing edge is the closer magnet: clip shifts so its OUT lands on the
    // target, i.e. tl_in = target - duration.
    const std::array<int64_t, 3> targets{40, 100, 180};
    // tl_in 130, duration 50 -> out 180 exactly = nailed target, identity.
    CHECK(timeline_snap::snap_dragged_edges(130, 50, targets, 0.5) == 130);
    // tl_in 135 -> out 185, 5 from 180 -> out snaps, in = 180-50 = 130.
    CHECK(timeline_snap::snap_dragged_edges(135, 50, targets, 0.5) == 130);
}

void test_dragged_edges_tie_and_closer_pull() {
    // Leading and trailing edge magnets both compete and the CLOSER pull wins;
    // an exact tie prefers the leading edge so an anchored head holds.
    const std::array<int64_t, 3> targets{79, 100, 130};
    // head 80 (->79, 1 frame) pulls while tail 130 rests exactly on a magnet:
    // the head snaps.
    CHECK(timeline_snap::snap_dragged_edges(80, 50, targets, 0.5) == 79);
    // head 81 (->79) vs tail 132 (->130): both 1 frame-ish... head 2, tail 2,
    // tie -> leading wins.
    CHECK(timeline_snap::snap_dragged_edges(81, 51, targets, 0.5) == 79);
    // tail strictly closer (d=1) than head (d=2): the OUT magnet wins and the
    // clip is laid so its out edge lands on 150 -> tl_in 99.
    const std::array<int64_t, 2> t2{100, 150};
    CHECK(timeline_snap::snap_dragged_edges(98, 51, t2, 0.5) == 99);
}

void test_dragged_edges_clamp_at_zero() {
    // Trailing-edge snap would push tl_in negative: clamps to 0.
    // raw 5, dur 14 -> tail 19 sits 6 frames from target 13, head 8 frames off
    // (fpp=1.0 -> radius 10): the OUT edge wins and tl_in = max(0, 13 - 14) = 0.
    CHECK(timeline_snap::snap_dragged_edges(5, 14, std::array<int64_t, 1>{13}, 1.0) == 0);
    // Magnets beyond the radius on both edges: unchanged.
    CHECK(timeline_snap::snap_dragged_edges(5, 3, std::array<int64_t, 1>{100}, 0.5) == 5);
    // No targets / degenerate zoom: untouched.
    CHECK(timeline_snap::snap_dragged_edges(5, 3, std::array<int64_t, 0>{}, 0.5) == 5);
}

void test_dragged_edges_inert_and_degenerate() {
    // Both edges beyond the radius: unchanged (grid fallback is the caller's).
    const std::array<int64_t, 2> targets{10, 500};
    CHECK(timeline_snap::snap_dragged_edges(200, 50, targets, 0.5) == 200);
    // Non-positive frames-per-pixel or empty targets disable magnetism.
    CHECK(timeline_snap::snap_dragged_edges(72, 50, targets, 0.0) == 72);
    CHECK(timeline_snap::snap_dragged_edges(72, 50, targets, -1.0) == 72);
    CHECK(timeline_snap::snap_dragged_edges(72, 50, std::array<int64_t, 0>{}, 0.5) == 72);
    // Degenerate duration must not divide/complicate.
    CHECK(timeline_snap::snap_dragged_edges(72, 0, targets, 0.5) == 72);
}

}  // namespace

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