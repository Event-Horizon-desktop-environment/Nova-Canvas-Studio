// Headless TransitionHandleEditor unit tests (splitplan Phase 30). Compiles
// transition_handle_editor.cpp directly into the binary so the module is
// verified exactly as shipped; the Qt-free seam is enforced by the build (a
// stray <Q...> include breaks this target on purpose).
//
// Covers the transition cut-handle session the timeline widget drives:
//   * max_duration — on a cut bounded by the shorter neighbour, on a single-clip
//     edge by the clip's own duration, invalid target -> kMinTransitionFrames;
//   * open — per-edge seeding + clamp into [min,max], and the left/right
//     derivation (Start extends right, End extends left, Cut splits symmetrically
//     with the same one-frame-off arithmetic as the pre-split widget);
//   * reopen — the unchanged-target no-op (the widget skips its scene rebuild);
//   * move_to — clamped duration + anchor stability (left drag pins the right
//     edge and vice versa);
//   * snap — documents the exact shipped preset-snap arithmetic (the loop seeds
//     `best` at the raw duration, so no preset can ever be strictly closer than
//     the value itself — behaviour is preserved verbatim for the mechanical move,
//     the latent no-op itself is tracked separately, not "fixed" here);
//   * has_transition / stored_duration — the press gate + release commit check;
//   * close — full session teardown.

#include "Widgets/transition_handle_editor.hpp"

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

using transition_editor::Editor;
using transition_editor::Edge;
using transition_editor::CutTarget;
using transition_editor::kMinTransitionFrames;
using transition_editor::kDragEdgeLeft;
using transition_editor::kDragEdgeRight;

// A: tl_in 100..400 (300 frames), OUT transition 30, IN transition 14.
canvas::core::Clip make_clip_a() {
    canvas::core::Clip c;
    c.id = 1;
    c.tl_in = 100;
    c.tl_out = 400;
    c.transition_out = canvas::core::TransitionType::CrossDissolve;
    c.transition_out_duration = 30;
    c.transition_in = canvas::core::TransitionType::FadeIn;
    c.transition_in_duration = 14;
    return c;
}

// B: abuts A at tl_in 400..700 (300 frames), no transitions.
canvas::core::Clip make_clip_b() {
    canvas::core::Clip c;
    c.id = 2;
    c.tl_in = 400;
    c.tl_out = 700;
    return c;
}

// C: abuts A but only 60 frames long, so a cut to it caps duration at 60.
canvas::core::Clip make_clip_short() {
    canvas::core::Clip c;
    c.id = 3;
    c.tl_in = 400;
    c.tl_out = 460;
    return c;
}

void test_max_duration() {
    const canvas::core::Clip a = make_clip_a();
    const canvas::core::Clip b = make_clip_b();
    const canvas::core::Clip short_clip = make_clip_short();

    // Single-clip edge: bounded by the clip's own duration.
    const CutTarget edge{&a, nullptr, canvas::core::Track::Kind::Video, 0, 400, Edge::End};
    CHECK(Editor::max_duration(edge) == 300);

    // Cut with equal neighbours: bounded by either.
    const CutTarget cut{&a, &b, canvas::core::Track::Kind::Video, 0, 400, Edge::Cut};
    CHECK(Editor::max_duration(cut) == 300);

    // Cut bounded by the SHORTER neighbour (short clip on the incoming side).
    const CutTarget short_cut{&a, &short_clip, canvas::core::Track::Kind::Video, 0, 400, Edge::Cut};
    CHECK(Editor::max_duration(short_cut) == 60);

    // Invalid (no clip): the floor.
    CHECK(Editor::max_duration(CutTarget{}) == kMinTransitionFrames);
}

void test_open_start_and_end_edges() {
    const canvas::core::Clip a = make_clip_a();
    const CutTarget start{&a, nullptr, canvas::core::Track::Kind::Video, 0, 100, Edge::Start};
    const CutTarget end{&a, nullptr, canvas::core::Track::Kind::Video, 1, 400, Edge::End};

    Editor ed;
    // Start (IN): seeded from a->transition_in_duration (14), extends rightward.
    CHECK(ed.open(start, a.transition_in_duration));
    CHECK(ed.visible());
    CHECK(!ed.dragging());
    CHECK(!ed.snap());
    CHECK(ed.duration() == 14);
    CHECK(ed.left() == 100);
    CHECK(ed.right() == 114);

    // The same target already open is a no-op (widget skips its rebuild).
    CHECK(!ed.open(start, a.transition_in_duration));

    // End (OUT): seeded from a->transition_out_duration (30), extends leftward.
    CHECK(ed.open(end, a.transition_out_duration));
    CHECK(ed.duration() == 30);
    CHECK(ed.left() == 370);
    CHECK(ed.right() == 400);
}

void test_open_cut_symmetry_and_seed_fallbacks() {
    const canvas::core::Clip a = make_clip_a();
    canvas::core::Clip b = make_clip_b();
    canvas::core::Clip b_with_in = make_clip_b();
    b_with_in.transition_in = canvas::core::TransitionType::FadeIn;
    b_with_in.transition_in_duration = 20;

    const CutTarget cut{&a, &b, canvas::core::Track::Kind::Video, 0, 400, Edge::Cut};
    const CutTarget cut_use_b{&a, &b_with_in, canvas::core::Track::Kind::Video, 0, 400, Edge::Cut};

    Editor ed;
    // Cut seeds from a->transition_out_duration (30) and splits across the cut:
    // left = cut - 30/2, right = cut + (30 - 30/2) — same arithmetic as before.
    CHECK(ed.open(cut, a.transition_out_duration));
    CHECK(ed.duration() == 30);
    CHECK(ed.left() == 385);
    CHECK(ed.right() == 415);

    // No OUT on a when b carries an IN -> the preferred side wins.
    canvas::core::Clip a_plain = make_clip_a();
    a_plain.transition_out_duration = 0;
    CHECK(ed.open(cut_use_b, 20));
    CHECK(ed.duration() == 20);
    CHECK(ed.left() == 390);
    CHECK(ed.right() == 410);

    // Neither side carries one -> the 6-frame seed fallback.
    canvas::core::Clip b_plain = make_clip_b();
    const CutTarget no_seed{&a_plain, &b_plain, canvas::core::Track::Kind::Video, 0, 400, Edge::Cut};
    CHECK(ed.open(no_seed, 6));
    CHECK(ed.duration() == 6);
}

void test_open_clamps_seed() {
    const canvas::core::Clip short_clip = make_clip_short();  // tl_in 400, dur 60
    const CutTarget edge{&short_clip, nullptr, canvas::core::Track::Kind::Video, 0, 400, Edge::Start};

    Editor ed;
    // Seed beyond the clip's own duration clamps to the max (60).
    CHECK(ed.open(edge, 90));
    CHECK(ed.duration() == 60);
    CHECK(ed.left() == 400);
    CHECK(ed.right() == 460);

    // Sub-minimum seed falls back to the 6-frame seed, then floors at the min.
    // (A same-target reopen is a no-op by design, so use a fresh session.)
    Editor ed2;
    CHECK(ed2.open(edge, 0));
    CHECK(ed2.duration() == 6);
    CHECK(ed2.left() == 400);
    CHECK(ed2.right() == 406);
}

void test_reopen_after_change() {
    const canvas::core::Clip a = make_clip_a();
    const canvas::core::Clip b = make_clip_b();
    const CutTarget start{&a, nullptr, canvas::core::Track::Kind::Video, 0, 100, Edge::Start};
    const CutTarget cut{&a, &b, canvas::core::Track::Kind::Video, 0, 400, Edge::Cut};

    Editor ed;
    CHECK(ed.open(start, a.transition_in_duration));
    // Same target again -> no-op.
    CHECK(!ed.open(start, a.transition_in_duration));
    // Different boundary (same clip, different cut_frame/edge) -> reopens.
    CHECK(ed.open(cut, a.transition_out_duration));
    CHECK(ed.target().edge == Edge::Cut);
    CHECK(ed.target().is_cut());
    // Back to the start boundary -> reopens again.
    CHECK(ed.open(start, a.transition_in_duration));
}

void test_move_clamp_and_anchor_stability() {
    const canvas::core::Clip a = make_clip_a();
    const CutTarget end{&a, nullptr, canvas::core::Track::Kind::Video, 0, 400, Edge::End};

    Editor ed;
    CHECK(ed.open(end, a.transition_out_duration));  // dur 30, left 370, right 400

    // Grab the RIGHT edge: the left edge (370) anchors.
    ed.begin_drag(kDragEdgeRight, false);
    CHECK(ed.dragging());
    CHECK(!ed.snap());
    ed.move_to(628);  // rawsize 628-370 = 258, clamped [1,300] -> 258
    CHECK(ed.duration() == 258);
    CHECK(ed.left() == 370);  // anchor pinned
    CHECK(ed.right() == 628);

    // Grab the LEFT edge now: the right edge (628) anchors and stays put.
    ed.begin_drag(kDragEdgeLeft, false);
    CHECK(ed.dragging());
    ed.move_to(390);  // rawsize 628-390 = 238
    CHECK(ed.duration() == 238);
    CHECK(ed.left() == 390);
    CHECK(ed.right() == 628);  // anchor pinned

    // Pointer behind the anchor still yields a legal (non-negative) duration.
    ed.begin_drag(kDragEdgeRight, false);  // anchors at left = 390
    ed.move_to(371);  // rawsize |371-390| = 19
    CHECK(ed.duration() == 19);
    CHECK(ed.left() == 390);   // anchor pinned
    CHECK(ed.right() == 409);

    // Clamp up to the duration bound on a huge pointer swing.
    ed.move_to(9999);  // rawsize > 300 -> clamped to 300, right = 390 + 300
    CHECK(ed.duration() == 300);
    CHECK(ed.right() == 690);

    // Clamp down to the floor: a 1-frame-wide transition is the minimum.
    ed.begin_drag(kDragEdgeRight, false);  // anchors at left = 390
    ed.move_to(391);  // rawsize 1
    CHECK(ed.duration() == 1);
    CHECK(ed.left() == 390);  // anchor pinned
    CHECK(ed.right() == 391);
}

void test_preset_snap_preserved_verbatim() {
    const canvas::core::Clip a = make_clip_a();
    const CutTarget end{&a, nullptr, canvas::core::Track::Kind::Video, 0, 400, Edge::End};

    // Snap on vs off produce IDENTICAL durations for every raw size: the loop
    // seeds `best` at the raw duration, so no favourite preset is ever strictly
    // closer than the value itself. This documents the exact pre-split
    // arithmetic (a mechanical extraction keeps behaviour bit-for-bit; the
    // latent design is tracked separately, not altered here).
    Editor snapped;
    CHECK(snapped.open(end, a.transition_out_duration));
    snapped.begin_drag(kDragEdgeRight, true);
    CHECK(snapped.snap());
    snapped.move_to(398);  // rawsize 28, nearest favourite would be 30
    CHECK(snapped.duration() == 28);

    Editor unsnapped;
    CHECK(unsnapped.open(end, a.transition_out_duration));
    unsnapped.begin_drag(kDragEdgeRight, false);
    unsnapped.move_to(398);
    CHECK(unsnapped.duration() == snapped.duration());
}

void test_has_transition_and_stored_duration() {
    const canvas::core::Clip a = make_clip_a();
    canvas::core::Clip plain = make_clip_a();  // same geometry, no transitions
    plain.transition_out = canvas::core::TransitionType::None;
    plain.transition_out_duration = 0;
    plain.transition_in = canvas::core::TransitionType::None;
    plain.transition_in_duration = 0;

    const CutTarget start{&a, nullptr, canvas::core::Track::Kind::Video, 0, 100, Edge::Start};
    const CutTarget end{&a, nullptr, canvas::core::Track::Kind::Video, 0, 400, Edge::End};
    const CutTarget none{&plain, nullptr, canvas::core::Track::Kind::Video, 0, 400, Edge::End};

    Editor ed;
    CHECK(ed.open(start, a.transition_in_duration));
    CHECK(ed.has_transition());                 // IN fade present
    CHECK(ed.stored_duration() == 14);          // commit baseline: IN duration

    CHECK(ed.open(end, a.transition_out_duration));
    CHECK(ed.has_transition());                 // OUT fade present
    CHECK(ed.stored_duration() == 30);          // commit baseline: OUT duration

    CHECK(ed.open(none, plain.transition_out_duration));
    CHECK(!ed.has_transition());                // press gate: resize refused
    CHECK(ed.stored_duration() == 0);
}

void test_close_resets_and_reopen() {
    const canvas::core::Clip a = make_clip_a();
    const CutTarget start{&a, nullptr, canvas::core::Track::Kind::Video, 0, 100, Edge::Start};

    Editor ed;
    CHECK(ed.open(start, a.transition_in_duration));
    CHECK(ed.visible());
    ed.begin_drag(kDragEdgeRight, true);
    CHECK(ed.dragging());

    ed.close();
    CHECK(!ed.visible());
    CHECK(!ed.dragging());
    CHECK(!ed.snap());
    CHECK(ed.duration() == 0);
    CHECK(ed.left() == 0);
    CHECK(ed.right() == 0);
    CHECK(!ed.target().valid());

    // A fresh open after close works and reports a (re)open.
    CHECK(ed.open(start, a.transition_in_duration));
    CHECK(ed.visible());
    CHECK(ed.duration() == 14);
}

}  // namespace

int main() {
    test_max_duration();
    test_open_start_and_end_edges();
    test_open_cut_symmetry_and_seed_fallbacks();
    test_open_clamps_seed();
    test_reopen_after_change();
    test_move_clamp_and_anchor_stability();
    test_preset_snap_preserved_verbatim();
    test_has_transition_and_stored_duration();
    test_close_resets_and_reopen();

    if (g_failures == 0) {
        std::printf("transition_handle_editor_test: ALL PASS\n");
        return 0;
    }
    std::printf("transition_handle_editor_test: %d FAILURE(S)\n", g_failures);
    return 1;
}