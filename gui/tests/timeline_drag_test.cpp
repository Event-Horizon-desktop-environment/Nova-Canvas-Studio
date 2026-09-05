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

    if (g_failures == 0) {
        std::printf("timeline_drag_test: ALL PASS\n");
        return 0;
    }
    std::printf("timeline_drag_test: %d FAILURE(S)\n", g_failures);
    return 1;
}