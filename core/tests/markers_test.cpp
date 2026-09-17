// Phase 1 (E4) marker / named-range tests. Pins:
//  - point markers (tl_out == 0) and the new named RANGES (tl_out > frame);
//  - sorted-by-start-frame invariants across point/range mixes;
//  - Sequence::bookmarks_in (the export-from-range query);
//  - markers::chapters_from (the D3 chapter table law);
//  - project JSON round-trip of markers + next_bookmark_id (historically NOT
//    serialized at all, so a save silently dropped every marker).
// Headless — links only canvas_core.

#include "canvas/core/project/project.hpp"
#include "canvas/core/timeline/markers.hpp"
#include "canvas/core/timeline/model.hpp"

#include <cstdio>
#include <string>
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

void test_points_and_ranges() {
    Sequence s;
    s.fps = 30.0;
    check(!s.has_bookmark(90), "no bookmark initially");

    const uint64_t p = s.toggle_bookmark(90, "Hit");
    check(p != 0 && s.has_bookmark(90), "point marker added");
    check(s.bookmarks.size() == 1 && !s.bookmarks[0].is_range(), "point marker is not a range");
    check(s.bookmarks[0].tl_out == 0, "point marker tl_out == 0");

    const uint64_t r = s.add_range(30, 60, "Intro");
    check(r != 0 && r != p, "range got a distinct id");
    check(s.bookmarks.size() == 2, "point + range counted");
    // Sorted by start frame: 30 then 90.
    check(s.bookmarks[0].frame == 30 && s.bookmarks[0].is_range(), "range sorts first");
    check(s.bookmarks[0].tl_out == 60, "range end stored");
    check(s.bookmarks[1].frame == 90, "point sorts after range");

    // Degenerate range (out <= in) becomes a point marker.
    const uint64_t d = s.add_range(45, 45, "deg");
    check(d != 0, "degenerate range id");
    for (const auto& b : s.bookmarks)
        if (b.id == d) check(!b.is_range() && b.tl_out == 0, "degenerate range = point");
    // Sorted: 30, 45, 90.
    check(s.bookmarks[0].frame == 30 && s.bookmarks[1].frame == 45 &&
              s.bookmarks[2].frame == 90,
          "bookmarks stay sorted by start frame");

    // toggle at an existing point frame REMOVES it.
    check(s.toggle_bookmark(90, "x") == 0, "toggle removes existing point");
    check(!s.has_bookmark(90), "point gone after toggle");
    check(s.remove_bookmark(r), "remove range by id");
    s.remove_bookmark_at(45);
    const bool removed_by_frame = s.bookmarks.empty() || s.bookmarks[0].frame != 45;
    check(removed_by_frame, "remove by frame");
    check(s.bookmarks.empty(), "all markers removed");
    check(!s.remove_bookmark(12345), "removing a bogus id fails");
}

void test_bookmarks_in() {
    Sequence s;
    s.fps = 30.0;
    (void)s.toggle_bookmark(0, "a");      // start in [0,100)
    (void)s.add_range(50, 70, "b");       // start in [0,100)
    (void)s.toggle_bookmark(100, "c");    // start NOT in [0,100)
    const auto in = s.bookmarks_in(0, 100);
    check(in.size() == 2, "bookmarks_in captures starts inside the window");
    check(in[0].frame == 0 && in[1].frame == 50, "in-range starts, sorted");

    const auto in2 = s.bookmarks_in(50, 101);
    check(in2.size() == 2, "window [50,101) captures 50 and 100");
    check(in2[0].frame == 50 && in2[1].frame == 100, "in2 starts");

    check(s.bookmarks_in(101, 200).empty(), "empty window");
}

void test_chapters() {
    Sequence s;
    s.fps = 30.0;
    check(markers::chapters_from(s).empty(), "no markers -> no chapters");

    (void)s.toggle_bookmark(90, "Scene 1");
    (void)s.add_range(300, 450, "Scene 2");
    (void)s.toggle_bookmark(600, "");  // empty label -> fallback

    const auto ch = markers::chapters_from(s);
    check(ch.size() == 3, "one chapter per bookmark");
    check(ch[0].seconds == 3.0, "90/30 = 3s");
    check(ch[0].label == "Scene 1", "label preserved");
    check(ch[1].seconds == 10.0, "range chapter at its START (300/30)");
    check(ch[2].seconds == 20.0, "600/30 = 20s");
    check(ch[2].label == "Chapter 3", "empty label falls back to Chapter N");

    Sequence bad;
    bad.fps = 0.0;
    (void)bad.toggle_bookmark(10, "x");
    check(markers::chapters_from(bad).empty(), "fps <= 0 -> empty chapter table");
}

void test_json_roundtrip() {
    Project p;
    p.name = "Markers";
    p.sequence.fps = 24.0;
    const uint64_t a = p.sequence.toggle_bookmark(48, "Act One");
    const uint64_t b = p.sequence.add_range(96, 192, "Montage");
    (void)a;
    (void)b;
    const uint64_t next_before = p.sequence.next_bookmark_id;

    const std::string path = "/tmp/canvas_markers_test.ncs";
    std::string err;
    check(save_project(p, path, &err), "save project with markers");
    Project q;
    check(load_project(q, path, &err), "load project with markers");

    check(q.sequence.bookmarks.size() == 2, "both markers round-trip");
    check(q.sequence.bookmarks[0].label == "Act One" && q.sequence.bookmarks[0].frame == 48,
          "point marker fields round-trip");
    check(q.sequence.bookmarks[1].is_range() && q.sequence.bookmarks[1].frame == 96 &&
              q.sequence.bookmarks[1].tl_out == 192,
          "range marker fields round-trip");
    check(q.sequence.next_bookmark_id == next_before, "next_bookmark_id round-trips");

    // A new marker must not collide with a loaded id.
    const uint64_t fresh = q.sequence.toggle_bookmark(1000, "new");
    bool collision = false;
    for (const auto& m : q.sequence.bookmarks)
        if (m.id == fresh && m.frame != 1000) collision = true;
    check(fresh != 0 && !collision, "new marker gets a fresh id after load");
}

}  // namespace

int main() {
    test_points_and_ranges();
    test_bookmarks_in();
    test_chapters();
    test_json_roundtrip();

    if (failures == 0) {
        std::printf("ALL TESTS PASSED\n");
        return 0;
    }
    std::fprintf(stderr, "%d FAILURES\n", failures);
    return 1;
}
