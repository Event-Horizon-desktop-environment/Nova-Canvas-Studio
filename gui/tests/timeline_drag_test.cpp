// Headless TimelineDragController unit tests (Phase 29). Compiles
// timeline_drag.cpp (+ timeline_snap.cpp) directly into the binary so the
// modules are verified exactly as shipped; the Qt-free seam is enforced by the
// build (a stray <Q...> include breaks this target on purpose).
//
// Covers the drag session the timeline widget drives:
//   * grab-frames — the press-time pointer offset into the clip is frozen so a
//     drag cannot make the clip jump under the cursor;
//   * snap-aware tl_in — grid quantization (fpp=4 -> 16-frame grid) with the
//     `snapped` cue set exactly when the raw position was quantised;
//   * clamp-to-zero — pointer behind the grab stays at tl_in 0;
//   * track resolution — same-kind track switches only, invalid/-1 candidates
//     retain the last valid target;
//   * commit — the release decision (something actually moved?) and final
//     snapped position.

#include "Widgets/timeline_drag.hpp"

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

// 2 video tracks (flat 0,1) + audio starting at flat 2.
constexpr int kVCount = 2;

void test_grab_frames_freeze() {
    timeline_drag::DragController ctrl;
    // Press 100 px-frames into the timeline on a clip starting at 60; the grab
    // offset is therefore 40 frames into the clip.
    ctrl.begin(100, 60, 0);
    CHECK(ctrl.active());

    // Pointer +50 -> clip should now sit at raw 50, NOT jump to 50+40.
    const auto r1 = ctrl.move(150, 0, kVCount, canvas::core::Track::Kind::Video,
                              false, 1.0);
    CHECK(r1.raw_tl_in == 110);
    CHECK(r1.new_tl_in == 110);
    CHECK(!r1.snapped);

    // Pointer back to the press spot resumes the original position exactly.
    const auto r2 = ctrl.move(100, 0, kVCount, canvas::core::Track::Kind::Video,
                              false, 1.0);
    CHECK(r2.raw_tl_in == 60);
    CHECK(r2.new_tl_in == 60);
}

void test_clamp_to_zero() {
    timeline_drag::DragController ctrl;
    ctrl.begin(100, 60, 0);
    // Pointer well LEFT of the press -> raw 30-40 = -10, clamped to 0.
    const auto r = ctrl.move(30, 0, kVCount, canvas::core::Track::Kind::Video,
                             false, 1.0);
    CHECK(r.new_tl_in == 0);
    CHECK(r.raw_tl_in == 0);
    CHECK(!r.snapped);
}

void test_snap_quantizes() {
    timeline_drag::DragController ctrl;
    ctrl.begin(100, 60, 0);
    // grab = 40, pointer 358 -> raw 318. fpp=4 -> grid step 16:
    // 318 snaps to 320 (nearest multiple of 16).
    const auto r = ctrl.move(358, 0, kVCount, canvas::core::Track::Kind::Video,
                             true, 4.0);
    CHECK(r.new_tl_in == 320);  // snap_to_grid(318, 4.0)
    CHECK(r.snapped);           // 318 != 320
    CHECK(r.raw_tl_in == 318);  // raw preserved for the indicator
}

void test_snap_disabled() {
    timeline_drag::DragController ctrl;
    ctrl.begin(100, 60, 0);
    const auto r = ctrl.move(358, 0, kVCount, canvas::core::Track::Kind::Video,
                             false, 4.0);
    CHECK(r.new_tl_in == 318);
    CHECK(!r.snapped);
}

void test_track_switch_same_kind_only() {
    // Video clip dragged onto the OTHER video track: allowed.
    timeline_drag::DragController ctrl;
    ctrl.begin(100, 100, 1);  // starts on flat track 1 (video)
    const auto r1 = ctrl.move(200, 0, kVCount, canvas::core::Track::Kind::Video, false, 1.0);
    (void)r1;
    CHECK(ctrl.target_track_index() == 0);

    // Video clip dragged onto an AUDIO track (flat 2): rejected, target stays.
    const auto r2 = ctrl.move(220, 2, kVCount, canvas::core::Track::Kind::Video, false, 1.0);
    (void)r2;
    CHECK(ctrl.target_track_index() == 0);

    // Audio clip dragged onto a VIDEO track (flat 0): rejected.
    timeline_drag::DragController a;
    a.begin(100, 100, 2);  // starts on flat 2 (audio)
    const auto r3 = a.move(200, 1, kVCount, canvas::core::Track::Kind::Audio, false, 1.0);
    (void)r3;
    CHECK(a.target_track_index() == 2);

    // Audio clip dragged onto another audio track (flat 3): allowed.
    const auto r4 = a.move(220, 3, kVCount, canvas::core::Track::Kind::Audio, false, 1.0);
    (void)r4;
    CHECK(a.target_track_index() == 3);
}

void test_invalid_candidate_keeps_target() {
    timeline_drag::DragController ctrl;
    ctrl.begin(100, 100, 1);
    // Valid video target first.
    const auto r1 = ctrl.move(200, 0, kVCount, canvas::core::Track::Kind::Video, false, 1.0);
    (void)r1;
    CHECK(ctrl.target_track_index() == 0);
    // Then no track under the cursor: the clip visual stays on the last valid.
    const auto r2 = ctrl.move(210, -1, kVCount, canvas::core::Track::Kind::Video, false, 1.0);
    (void)r2;
    CHECK(ctrl.target_track_index() == 0);
}

void test_commit_noop_vs_moved() {
    // Returned to the origin, same track -> nothing happened.
    timeline_drag::DragController ctrl;
    ctrl.begin(100, 60, 0);
    const auto m1 = ctrl.move(200, 0, kVCount, canvas::core::Track::Kind::Video, false, 1.0);
    (void)m1;
    const auto m2 = ctrl.move(100, 0, kVCount, canvas::core::Track::Kind::Video, false, 1.0);
    (void)m2;
    const auto c1 = ctrl.commit(100, 60, false, 1.0);
    CHECK(c1.new_tl_in == 60);
    CHECK(!c1.changed);

    // Moved in time -> commit fires.
    timeline_drag::DragController c2;
    c2.begin(100, 60, 0);
    const auto m3 = c2.move(200, 0, kVCount, canvas::core::Track::Kind::Video, false, 1.0);
    (void)m3;
    const auto r2 = c2.commit(200, 60, false, 1.0);
    CHECK(r2.new_tl_in == 160);
    CHECK(r2.changed);
}

void test_commit_track_change_only() {
    // Same tl_in but a different track still counts as a move.
    timeline_drag::DragController ctrl;
    ctrl.begin(100, 100, 0);
    const auto m = ctrl.move(100, 1, kVCount, canvas::core::Track::Kind::Video, false, 1.0);
    (void)m;
    const auto r = ctrl.commit(100, 100, false, 1.0);
    CHECK(r.new_tl_in == 100);
    CHECK(r.changed);
}

void test_commit_snaps_release_position() {
    timeline_drag::DragController ctrl;
    ctrl.begin(100, 60, 0);
    const auto m = ctrl.move(358, 0, kVCount, canvas::core::Track::Kind::Video, true, 4.0);
    (void)m;
    // Release pointer 360 -> raw 320, exact grid multiple, stays 320.
    const auto r = ctrl.commit(360, 60, true, 4.0);
    CHECK(r.new_tl_in == 320);
    CHECK(r.changed);
}

void test_end_resets() {
    timeline_drag::DragController ctrl;
    ctrl.begin(100, 60, 0);
    CHECK(ctrl.active());
    ctrl.end();
    CHECK(!ctrl.active());
    // A fresh begin rebinds cleanly (grab = 50 - 50 = 0 here).
    ctrl.begin(50, 50, 2);
    CHECK(ctrl.active());
    const auto r = ctrl.move(110, 2, kVCount, canvas::core::Track::Kind::Audio,
                             false, 1.0);
    CHECK(r.new_tl_in == 110);
}

void test_batch_delta_preserved() {
    // Batch drag math (Phase 3): primary 60 -> 320 (snapped), so every other
    // selected clip shifts by +260 on its own lane; relative spacings hold.
    CHECK(timeline_drag::batch_target_tl_in(60, 320, 200) == 460);
    CHECK(timeline_drag::batch_target_tl_in(60, 320, 90) == 350);
    // Negative delta vs. an early clip clamps at 0.
    CHECK(timeline_drag::batch_target_tl_in(300, 100, 40) == 0);
    // Zero delta (no actual move) maps every clip back to its origin.
    CHECK(timeline_drag::batch_target_tl_in(120, 120, 40) == 40);
    // Primary moved EARLIER: later clips follow the same negative delta.
    CHECK(timeline_drag::batch_target_tl_in(600, 300, 720) == 420);
}

// --- Resolve-style magnetic edge snapping (Phase 8) ---------------------------

void test_edge_snap_leading_edge_pulls() {
    // The widget passes the sorted snap targets (other clips' edges, playhead,
    // bookmarks) into move/commit; the DragController magnetises the dragged
    // clip's edges onto them FIRST, keeping the grid as the zoomed-out fallback.
    timeline_drag::DragController ctrl;
    // begin(160, tl_in=100, dur=40) -> grab offset 60 frames.
    ctrl.begin(160, 100, 0, 40);
    // A neighbour's in-edge at 500. fpp=2.0 -> radius llround(10*2)=20 frames
    // (the 10px magnet, frames_per_pixel being frames PER pixel). pointer 567
    // -> raw 507, head 7 frames from 500 (tail 547 is inert, 47 away): the
    // HEAD snaps to 500 despite being off-grid.
    const std::array<int64_t, 1> targets{500};
    const auto r = ctrl.move(567, 0, kVCount, canvas::core::Track::Kind::Video, true,
                             2.0, targets);
    CHECK(r.raw_tl_in == 507);
    CHECK(r.new_tl_in == 500);
    CHECK(r.snapped);
}

void test_edge_snap_trailing_edge_pulls() {
    timeline_drag::DragController ctrl;
    ctrl.begin(160, 100, 0, 40);  // grab 60
    // pointer 612 -> raw 552 (tail 592), 8 frames from the 600 magnet; head is
    // 48 away and inert, so the OUT edge wins and lays the clip at 560.
    const std::array<int64_t, 1> targets{600};
    const auto r = ctrl.move(612, 0, kVCount, canvas::core::Track::Kind::Video, true,
                             2.0, targets);
    CHECK(r.raw_tl_in == 552);
    CHECK(r.new_tl_in == 560);
    CHECK(r.snapped);
}

void test_edge_snap_grid_fallback() {
    // No edge target within the radius -> plain grid quantization as before.
    timeline_drag::DragController ctrl;
    ctrl.begin(160, 100, 0, 40);
    // fpp=4 -> 16-frame grid, radius llround(10*4)=40 frames; the 700 target
    // is far outside the radius on both edges, so 318 quantizes to 320.
    const std::array<int64_t, 1> targets{700};
    const auto r = ctrl.move(378, 0, kVCount, canvas::core::Track::Kind::Video, true,
                             4.0, targets);
    CHECK(r.raw_tl_in == 318);
    CHECK(r.new_tl_in == 320);
    CHECK(r.snapped);
}

void test_edge_snap_disabled_and_collection() {
    // Snap off: edge targets are ignored entirely.
    timeline_drag::DragController ctrl;
    ctrl.begin(160, 100, 0, 40);
    const std::array<int64_t, 1> targets{500};
    const auto r = ctrl.move(567, 0, kVCount, canvas::core::Track::Kind::Video, false,
                             0.5, targets);
    CHECK(r.new_tl_in == 507);
    CHECK(!r.snapped);

    // Empty target span (e.g. single-clip timeline) degrades to grid snapping.
    const std::array<int64_t, 0> none{};
    const auto r2 = ctrl.move(358, 0, kVCount, canvas::core::Track::Kind::Video, true,
                              4.0, none);
    CHECK(r2.new_tl_in == 304);  // raw 298 -> grid 16 -> 304
}

void test_edge_snap_commit_resolves() {
    timeline_drag::DragController ctrl;
    ctrl.begin(160, 100, 0, 40);
    const std::array<int64_t, 1> targets{500};
    // Release pointer 567 -> snapped head 500 != press tl_in 100: real move.
    const auto r = ctrl.commit(567, 100, true, 2.0, targets);
    CHECK(r.new_tl_in == 500);
    CHECK(r.changed);
}

}  // namespace

int main() {
    test_grab_frames_freeze();
    test_clamp_to_zero();
    test_snap_quantizes();
    test_snap_disabled();
    test_track_switch_same_kind_only();
    test_invalid_candidate_keeps_target();
    test_commit_noop_vs_moved();
    test_commit_track_change_only();
    test_commit_snaps_release_position();
    test_end_resets();
    test_batch_delta_preserved();
    test_edge_snap_leading_edge_pulls();
    test_edge_snap_trailing_edge_pulls();
    test_edge_snap_grid_fallback();
    test_edge_snap_disabled_and_collection();
    test_edge_snap_commit_resolves();

    if (g_failures == 0) {
        std::printf("timeline_drag_test: ALL PASS\n");
        return 0;
    }
    std::printf("timeline_drag_test: %d FAILURE(S)\n", g_failures);
    return 1;
}