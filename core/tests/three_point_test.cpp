// Phase 1 (E2) 3-point editing tests. Pins the source-window → clip geometry
// law (timeline/three_point.hpp) and the two placement teeth that make a
// 3-point edit meaningful:
//  - INSERT ripples: content at/after the in-point shifts right by the source
//    window's duration, and a clip straddling the in-point is SPLIT (nothing is
//    silently covered);
//  - OVERWRITE replaces the overlapped span and splits the neighbour, shifting
//    nothing else;
//  - both honour the time-based duration law (src span * seq.fps / media.fps);
//  - linked A/V variants do both tracks together;
//  - every placement is one undoable command.
// Headless — links only canvas_core.

#include "canvas/core/timeline/edit_ops.hpp"
#include "canvas/core/timeline/model.hpp"
#include "canvas/core/timeline/three_point.hpp"

#include <algorithm>
#include <cstdio>
#include <vector>

using namespace canvas::core;

namespace {

int failures = 0;

void check(const bool cond, const char* what) {
    if (!cond) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++failures;
    } else {
        std::printf("ok:   %s\n", what);
    }
}

struct Span {
    int64_t in = 0;
    int64_t out = 0;
    MediaId media = -1;
};

std::vector<Span> spans(const Track& t) {
    std::vector<Span> v;
    for (const auto& c : t.clips) v.push_back(Span{c.tl_in, c.tl_out, c.media});
    std::sort(v.begin(), v.end(),
              [](const Span& a, const Span& b) { return a.in < b.in; });
    return v;
}

Clip existing(const MediaId media, const int64_t in, const int64_t out, const ClipId id) {
    Clip c;
    c.id = id;
    c.media = media;
    c.tl_in = in;
    c.tl_out = out;
    c.src_in = 0;
    c.src_out = out - in;
    return c;
}

void test_duration_law() {
    three_point::Marks m;
    m.src_in = 10;
    m.src_out = 40;
    m.tl_in = 0;
    check(m.src_span() == 30, "src_span");

    check(three_point::timeline_duration(m, 30.0, 30.0) == 30, "same fps -> span");
    check(three_point::timeline_duration(m, 30.0, 60.0) == 15, "30 on 60fps media -> half");
    check(three_point::timeline_duration(m, 60.0, 30.0) == 60, "60 on 30fps media -> double");
    check(three_point::timeline_duration(m, 30.0, 0.0) == 30, "unknown media fps -> span");

    three_point::Marks inv;
    inv.src_in = 40;
    inv.src_out = 10;
    check(inv.src_span() == 0, "inverted window -> empty span");
    Clip c = three_point::make_clip(5, inv, 30.0, 30.0);
    check(c.tl_out == c.tl_in, "inverted window -> zero-length clip");

    Clip d = three_point::make_clip(5, m, 30.0, 60.0);
    check(d.media == 5 && d.src_in == 10 && d.src_out == 40 && d.tl_in == 0 && d.tl_out == 15,
          "make_clip geometry at media fps 60");
}

void test_insert_ripples_and_splits() {
    Sequence s;
    s.fps = 30.0;
    Track v;
    v.kind = Track::Kind::Video;
    v.name = "V1";
    v.clips.push_back(existing(1, 0, 100, 1));
    s.video_tracks.push_back(v);
    s.next_clip_id = 2;

    three_point::Marks m;
    m.src_in = 0;
    m.src_out = 30;
    m.tl_in = 50;
    Clip nc = three_point::make_clip(2, m, 30.0, 30.0);

    auto cmd = place_clip(s, Track::Kind::Video, 0, nc, Placement::Insert, 30.0);
    check(cmd != nullptr, "insert command");
    auto sp = spans(s.video_tracks[0]);
    check(sp.size() == 3, "insert splits the straddled clip into 3");
    check(sp[0].in == 0 && sp[0].out == 50 && sp[0].media == 1, "head kept [0,50)");
    check(sp[1].in == 50 && sp[1].out == 80 && sp[1].media == 2, "new clip [50,80)");
    check(sp[2].in == 80 && sp[2].out == 130 && sp[2].media == 1, "tail rippled to [80,130)");

    cmd->undo(s);
    auto undo_sp = spans(s.video_tracks[0]);
    check(undo_sp.size() == 1 && undo_sp[0].in == 0 && undo_sp[0].out == 100,
          "undo restores the single original clip");
    cmd->redo(s);
    check(spans(s.video_tracks[0]).size() == 3, "redo re-applies the insert");
}

void test_overwrite_replaces() {
    Sequence s;
    s.fps = 30.0;
    Track v;
    v.kind = Track::Kind::Video;
    v.name = "V1";
    v.clips.push_back(existing(1, 0, 100, 1));
    s.video_tracks.push_back(v);
    s.next_clip_id = 2;

    three_point::Marks m;
    m.src_in = 0;
    m.src_out = 30;
    m.tl_in = 30;
    Clip nc = three_point::make_clip(2, m, 30.0, 30.0);

    auto cmd = place_clip(s, Track::Kind::Video, 0, nc, Placement::Overwrite, 30.0);
    check(cmd != nullptr, "overwrite command");
    auto sp = spans(s.video_tracks[0]);
    check(sp.size() == 3, "overwrite splits the covered clip");
    check(sp[0].in == 0 && sp[0].out == 30 && sp[0].media == 1, "head [0,30) untouched");
    check(sp[1].in == 30 && sp[1].out == 60 && sp[1].media == 2, "new clip [30,60)");
    check(sp[2].in == 60 && sp[2].out == 100 && sp[2].media == 1, "tail [60,100) not shifted");

    cmd->undo(s);
    check(spans(s.video_tracks[0]).size() == 1, "overwrite undo restores");
}

void test_linked_insert() {
    Sequence s;
    s.fps = 30.0;
    Track v;
    v.kind = Track::Kind::Video;
    v.name = "V1";
    v.clips.push_back(existing(1, 0, 100, 1));
    Track a;
    a.kind = Track::Kind::Audio;
    a.name = "A1";
    a.clips.push_back(existing(2, 0, 100, 2));
    s.video_tracks.push_back(v);
    s.audio_tracks.push_back(a);
    s.next_clip_id = 3;

    three_point::Marks mv;
    mv.src_in = 0;
    mv.src_out = 20;
    mv.tl_in = 40;
    Clip nv = three_point::make_clip(3, mv, 30.0, 30.0);
    three_point::Marks ma = mv;
    Clip na = three_point::make_clip(4, ma, 30.0, 30.0);

    auto cmd = place_linked_clip(s, 0, 0, nv, na, Placement::Insert, 30.0);
    check(cmd != nullptr, "linked insert command");

    auto vsp = spans(s.video_tracks[0]);
    auto asp = spans(s.audio_tracks[0]);
    check(vsp.size() == 3 && asp.size() == 3, "both tracks split");
    check(vsp[1].in == 40 && vsp[1].out == 60 && vsp[1].media == 3, "video insert [40,60)");
    check(asp[1].in == 40 && asp[1].out == 60 && asp[1].media == 4, "audio insert [40,60)");
    check(vsp[2].in == 60 && vsp[2].out == 120, "video tail rippled");
    check(asp[2].in == 60 && asp[2].out == 120, "audio tail rippled");

    cmd->undo(s);
    check(spans(s.video_tracks[0]).size() == 1 && spans(s.audio_tracks[0]).size() == 1,
          "linked undo restores both tracks");
}

}  // namespace

int main() {
    test_duration_law();
    test_insert_ripples_and_splits();
    test_overwrite_replaces();
    test_linked_insert();

    if (failures == 0) {
        std::printf("ALL TESTS PASSED\n");
        return 0;
    }
    std::fprintf(stderr, "%d FAILURES\n", failures);
    return 1;
}
