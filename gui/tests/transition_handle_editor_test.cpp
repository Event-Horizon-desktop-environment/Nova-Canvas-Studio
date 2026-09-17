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

canvas::core::Clip make_clip_b() {
    canvas::core::Clip c;
    c.id = 2;
    c.tl_in = 400;
    c.tl_out = 700;
    return c;
}

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

    const CutTarget edge{&a, nullptr, canvas::core::Track::Kind::Video, 0, 400, Edge::End};
    CHECK(Editor::max_duration(edge) == 300);

    const CutTarget cut{&a, &b, canvas::core::Track::Kind::Video, 0, 400, Edge::Cut};
    CHECK(Editor::max_duration(cut) == 300);

    const CutTarget short_cut{&a, &short_clip, canvas::core::Track::Kind::Video, 0, 400, Edge::Cut};
    CHECK(Editor::max_duration(short_cut) == 60);

    CHECK(Editor::max_duration(CutTarget{}) == kMinTransitionFrames);
}

void test_open_start_and_end_edges() {
    const canvas::core::Clip a = make_clip_a();
    const CutTarget start{&a, nullptr, canvas::core::Track::Kind::Video, 0, 100, Edge::Start};
    const CutTarget end{&a, nullptr, canvas::core::Track::Kind::Video, 1, 400, Edge::End};

    Editor ed;
    CHECK(ed.open(start, a.transition_in_duration));
    CHECK(ed.visible());
    CHECK(!ed.dragging());
    CHECK(!ed.snap());
    CHECK(ed.duration() == 14);
    CHECK(ed.left() == 100);
    CHECK(ed.right() == 114);

    CHECK(!ed.open(start, a.transition_in_duration));

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
    CHECK(ed.open(cut, a.transition_out_duration));
    CHECK(ed.duration() == 30);
    CHECK(ed.left() == 385);
    CHECK(ed.right() == 415);

    canvas::core::Clip a_plain = make_clip_a();
    a_plain.transition_out_duration = 0;
    CHECK(ed.open(cut_use_b, 20));
    CHECK(ed.duration() == 20);
    CHECK(ed.left() == 390);
    CHECK(ed.right() == 410);

    canvas::core::Clip b_plain = make_clip_b();
    const CutTarget no_seed{&a_plain, &b_plain, canvas::core::Track::Kind::Video, 0, 400, Edge::Cut};
    CHECK(ed.open(no_seed, 6));
    CHECK(ed.duration() == 6);
}

void test_open_clamps_seed() {
    const canvas::core::Clip short_clip = make_clip_short();
    const CutTarget edge{&short_clip, nullptr, canvas::core::Track::Kind::Video, 0, 400, Edge::Start};

    Editor ed;
    CHECK(ed.open(edge, 90));
    CHECK(ed.duration() == 60);
    CHECK(ed.left() == 400);
    CHECK(ed.right() == 460);

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
    CHECK(!ed.open(start, a.transition_in_duration));
    CHECK(ed.open(cut, a.transition_out_duration));
    CHECK(ed.target().edge == Edge::Cut);
    CHECK(ed.target().is_cut());
    CHECK(ed.open(start, a.transition_in_duration));
}

void test_move_clamp_and_anchor_stability() {
    const canvas::core::Clip a = make_clip_a();
    const CutTarget end{&a, nullptr, canvas::core::Track::Kind::Video, 0, 400, Edge::End};

    Editor ed;
    CHECK(ed.open(end, a.transition_out_duration));

    ed.begin_drag(kDragEdgeRight, false);
    CHECK(ed.dragging());
    CHECK(!ed.snap());
    ed.move_to(628);
    CHECK(ed.duration() == 258);
    CHECK(ed.left() == 370);
    CHECK(ed.right() == 628);

    ed.begin_drag(kDragEdgeLeft, false);
    CHECK(ed.dragging());
    ed.move_to(390);
    CHECK(ed.duration() == 238);
    CHECK(ed.left() == 390);
    CHECK(ed.right() == 628);

    ed.begin_drag(kDragEdgeRight, false);
    ed.move_to(371);
    CHECK(ed.duration() == 19);
    CHECK(ed.left() == 390);
    CHECK(ed.right() == 409);

    ed.move_to(9999);
    CHECK(ed.duration() == 300);
    CHECK(ed.right() == 690);

    ed.begin_drag(kDragEdgeRight, false);
    ed.move_to(391);
    CHECK(ed.duration() == 1);
    CHECK(ed.left() == 390);
    CHECK(ed.right() == 391);
}

void test_preset_snap_preserved_verbatim() {
    const canvas::core::Clip a = make_clip_a();
    const CutTarget end{&a, nullptr, canvas::core::Track::Kind::Video, 0, 400, Edge::End};

    Editor snapped;
    CHECK(snapped.open(end, a.transition_out_duration));
    snapped.begin_drag(kDragEdgeRight, true);
    CHECK(snapped.snap());
    snapped.move_to(398);
    CHECK(snapped.duration() == 28);

    Editor unsnapped;
    CHECK(unsnapped.open(end, a.transition_out_duration));
    unsnapped.begin_drag(kDragEdgeRight, false);
    unsnapped.move_to(398);
    CHECK(unsnapped.duration() == snapped.duration());
}

void test_has_transition_and_stored_duration() {
    const canvas::core::Clip a = make_clip_a();
    canvas::core::Clip plain = make_clip_a();
    plain.transition_out = canvas::core::TransitionType::None;
    plain.transition_out_duration = 0;
    plain.transition_in = canvas::core::TransitionType::None;
    plain.transition_in_duration = 0;

    const CutTarget start{&a, nullptr, canvas::core::Track::Kind::Video, 0, 100, Edge::Start};
    const CutTarget end{&a, nullptr, canvas::core::Track::Kind::Video, 0, 400, Edge::End};
    const CutTarget none{&plain, nullptr, canvas::core::Track::Kind::Video, 0, 400, Edge::End};

    Editor ed;
    CHECK(ed.open(start, a.transition_in_duration));
    CHECK(ed.has_transition());
    CHECK(ed.stored_duration() == 14);

    CHECK(ed.open(end, a.transition_out_duration));
    CHECK(ed.has_transition());
    CHECK(ed.stored_duration() == 30);

    CHECK(ed.open(none, plain.transition_out_duration));
    CHECK(!ed.has_transition());
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

    CHECK(ed.open(start, a.transition_in_duration));
    CHECK(ed.visible());
    CHECK(ed.duration() == 14);
}

}

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