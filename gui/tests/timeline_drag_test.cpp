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

constexpr int kVCount = 2;

void test_grab_frames_freeze() {
    timeline_drag::DragController ctrl;
    ctrl.begin(100, 60, 0);
    CHECK(ctrl.active());

    const auto r1 = ctrl.move(150, 0, kVCount, canvas::core::Track::Kind::Video,
                              false, 1.0);
    CHECK(r1.raw_tl_in == 110);
    CHECK(r1.new_tl_in == 110);
    CHECK(!r1.snapped);

    const auto r2 = ctrl.move(100, 0, kVCount, canvas::core::Track::Kind::Video,
                              false, 1.0);
    CHECK(r2.raw_tl_in == 60);
    CHECK(r2.new_tl_in == 60);
}

void test_clamp_to_zero() {
    timeline_drag::DragController ctrl;
    ctrl.begin(100, 60, 0);
    const auto r = ctrl.move(30, 0, kVCount, canvas::core::Track::Kind::Video,
                             false, 1.0);
    CHECK(r.new_tl_in == 0);
    CHECK(r.raw_tl_in == 0);
    CHECK(!r.snapped);
}

void test_snap_quantizes() {
    timeline_drag::DragController ctrl;
    ctrl.begin(100, 60, 0);
    const auto r = ctrl.move(358, 0, kVCount, canvas::core::Track::Kind::Video,
                             true, 4.0);
    CHECK(r.new_tl_in == 320);
    CHECK(r.snapped);
    CHECK(r.raw_tl_in == 318);
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
    timeline_drag::DragController ctrl;
    ctrl.begin(100, 100, 1);
    const auto r1 = ctrl.move(200, 0, kVCount, canvas::core::Track::Kind::Video, false, 1.0);
    (void)r1;
    CHECK(ctrl.target_track_index() == 0);

    const auto r2 = ctrl.move(220, 2, kVCount, canvas::core::Track::Kind::Video, false, 1.0);
    (void)r2;
    CHECK(ctrl.target_track_index() == 0);

    timeline_drag::DragController a;
    a.begin(100, 100, 2);
    const auto r3 = a.move(200, 1, kVCount, canvas::core::Track::Kind::Audio, false, 1.0);
    (void)r3;
    CHECK(a.target_track_index() == 2);

    const auto r4 = a.move(220, 3, kVCount, canvas::core::Track::Kind::Audio, false, 1.0);
    (void)r4;
    CHECK(a.target_track_index() == 3);
}

void test_invalid_candidate_keeps_target() {
    timeline_drag::DragController ctrl;
    ctrl.begin(100, 100, 1);
    const auto r1 = ctrl.move(200, 0, kVCount, canvas::core::Track::Kind::Video, false, 1.0);
    (void)r1;
    CHECK(ctrl.target_track_index() == 0);
    const auto r2 = ctrl.move(210, -1, kVCount, canvas::core::Track::Kind::Video, false, 1.0);
    (void)r2;
    CHECK(ctrl.target_track_index() == 0);
}

void test_commit_noop_vs_moved() {
    timeline_drag::DragController ctrl;
    ctrl.begin(100, 60, 0);
    const auto m1 = ctrl.move(200, 0, kVCount, canvas::core::Track::Kind::Video, false, 1.0);
    (void)m1;
    const auto m2 = ctrl.move(100, 0, kVCount, canvas::core::Track::Kind::Video, false, 1.0);
    (void)m2;
    const auto c1 = ctrl.commit(100, 60, false, 1.0);
    CHECK(c1.new_tl_in == 60);
    CHECK(!c1.changed);

    timeline_drag::DragController c2;
    c2.begin(100, 60, 0);
    const auto m3 = c2.move(200, 0, kVCount, canvas::core::Track::Kind::Video, false, 1.0);
    (void)m3;
    const auto r2 = c2.commit(200, 60, false, 1.0);
    CHECK(r2.new_tl_in == 160);
    CHECK(r2.changed);
}

void test_commit_track_change_only() {
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
    ctrl.begin(50, 50, 2);
    CHECK(ctrl.active());
    const auto r = ctrl.move(110, 2, kVCount, canvas::core::Track::Kind::Audio,
                             false, 1.0);
    CHECK(r.new_tl_in == 110);
}

void test_batch_delta_preserved() {
    CHECK(timeline_drag::batch_target_tl_in(60, 320, 200, 40) == 460);
    CHECK(timeline_drag::batch_target_tl_in(60, 320, 90, 40) == 350);
    CHECK(timeline_drag::clamp_batch_delta(300, 100, 40) == -40);
    CHECK(timeline_drag::batch_target_tl_in(300, 100, 40, 40) == 0);
    CHECK(timeline_drag::batch_target_tl_in(120, 120, 40, 40) == 40);
    CHECK(timeline_drag::batch_target_tl_in(600, 300, 720, 600) == 420);
    CHECK(timeline_drag::batch_target_tl_in(300, 100, 720, 40) == 680);
}

void test_edge_snap_leading_edge_pulls() {
    timeline_drag::DragController ctrl;
    ctrl.begin(160, 100, 0, 40);
    const std::array<int64_t, 1> targets{500};
    const auto r = ctrl.move(567, 0, kVCount, canvas::core::Track::Kind::Video, true,
                             2.0, targets);
    CHECK(r.raw_tl_in == 507);
    CHECK(r.new_tl_in == 500);
    CHECK(r.snapped);
}

void test_edge_snap_trailing_edge_pulls() {
    timeline_drag::DragController ctrl;
    ctrl.begin(160, 100, 0, 40);
    const std::array<int64_t, 1> targets{600};
    const auto r = ctrl.move(612, 0, kVCount, canvas::core::Track::Kind::Video, true,
                             2.0, targets);
    CHECK(r.raw_tl_in == 552);
    CHECK(r.new_tl_in == 560);
    CHECK(r.snapped);
}

void test_edge_snap_grid_fallback() {
    timeline_drag::DragController ctrl;
    ctrl.begin(160, 100, 0, 40);
    const std::array<int64_t, 1> targets{700};
    const auto r = ctrl.move(378, 0, kVCount, canvas::core::Track::Kind::Video, true,
                             4.0, targets);
    CHECK(r.raw_tl_in == 318);
    CHECK(r.new_tl_in == 320);
    CHECK(r.snapped);
}

void test_edge_snap_disabled_and_collection() {
    timeline_drag::DragController ctrl;
    ctrl.begin(160, 100, 0, 40);
    const std::array<int64_t, 1> targets{500};
    const auto r = ctrl.move(567, 0, kVCount, canvas::core::Track::Kind::Video, false,
                             0.5, targets);
    CHECK(r.new_tl_in == 507);
    CHECK(!r.snapped);

    const std::array<int64_t, 0> none{};
    const auto r2 = ctrl.move(358, 0, kVCount, canvas::core::Track::Kind::Video, true,
                              4.0, none);
    CHECK(r2.new_tl_in == 304);
}

void test_edge_snap_commit_resolves() {
    timeline_drag::DragController ctrl;
    ctrl.begin(160, 100, 0, 40);
    const std::array<int64_t, 1> targets{500};
    const auto r = ctrl.commit(567, 100, true, 2.0, targets);
    CHECK(r.new_tl_in == 500);
    CHECK(r.changed);
}

}

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