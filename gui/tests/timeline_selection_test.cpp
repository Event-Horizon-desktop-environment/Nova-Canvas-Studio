#include "Widgets/timeline_selection.hpp"

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

bool vec_eq(const std::vector<canvas::core::ClipId>& a, const std::vector<canvas::core::ClipId>& b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i)
        if (a[i] != b[i]) return false;
    return true;
}

canvas::core::Sequence make_seq() {
    canvas::core::Sequence s;
    canvas::core::Track v0;
    v0.kind = canvas::core::Track::Kind::Video;
    v0.clips = {
        {.id = 1, .media = -1, .tl_in = 0, .tl_out = 100, .src_in = 0, .src_out = 100, .linked_id = 3},
        {.id = 2, .media = -1, .tl_in = 100, .tl_out = 200, .src_in = 0, .src_out = 100},
    };
    canvas::core::Track a0;
    a0.kind = canvas::core::Track::Kind::Audio;
    a0.clips = {
        {.id = 3, .media = -1, .tl_in = 0, .tl_out = 100, .src_in = 0, .src_out = 100, .linked_id = 1},
    };
    s.video_tracks.push_back(v0);
    s.audio_tracks.push_back(a0);
    return s;
}

void test_clips_in_range_basics() {
    const auto s = make_seq();
    CHECK(vec_eq(timeline_selection::clips_in_range(s, 50, 99), {1, 3}));
    CHECK(vec_eq(timeline_selection::clips_in_range(s, 0, 100), {1, 3}));
    CHECK(vec_eq(timeline_selection::clips_in_range(s, 100, 200), {2}));
    CHECK(vec_eq(timeline_selection::clips_in_range(s, 10, 11), {1, 3}));
}

void test_clips_in_range_boundaries() {
    const auto s = make_seq();
    CHECK(vec_eq(timeline_selection::clips_in_range(s, 100, 101), {2}));
    CHECK(vec_eq(timeline_selection::clips_in_range(s, 0, 1), {1, 3}));
    CHECK(timeline_selection::clips_in_range(s, 300, 400).empty());
    CHECK(timeline_selection::clips_in_range(s, 150, 100).empty());
    CHECK(vec_eq(timeline_selection::clips_in_range(s, 50, 50), {1, 3}));
}

void test_clips_in_range_empty_sequence() {
    const canvas::core::Sequence s;
    CHECK(timeline_selection::clips_in_range(s, 0, 1000).empty());
}

void test_expand_with_mates() {
    const auto s = make_seq();
    CHECK(vec_eq(timeline_selection::expand_with_mates({1}, s), {1, 3}));
    CHECK(vec_eq(timeline_selection::expand_with_mates({3}, s), {3, 1}));
    CHECK(vec_eq(timeline_selection::expand_with_mates({2}, s), {2}));
    CHECK(vec_eq(timeline_selection::expand_with_mates({1, 3}, s), {1, 3}));
    CHECK(vec_eq(timeline_selection::expand_with_mates({2, 1}, s), {2, 1, 3}));
    CHECK(timeline_selection::expand_with_mates({}, s).empty());
    CHECK(vec_eq(timeline_selection::expand_with_mates({777}, s), {777}));
}

void test_expand_with_mates_empty_sequence() {
    const canvas::core::Sequence s;
    CHECK(vec_eq(timeline_selection::expand_with_mates({4, 5}, s), {4, 5}));
}

void test_selection_state_basics() {
    const auto s = make_seq();
    timeline_selection::SelectionState sel;
    CHECK(sel.size() == 0);
    CHECK(!sel.contains(1));

    sel.set({1}, &s);
    CHECK(sel.size() == 2);
    CHECK(sel.contains(1));
    CHECK(sel.contains(3));

    sel.set({2}, &s);
    CHECK(sel.size() == 1);
    CHECK(sel.contains(2));
    CHECK(!sel.contains(1));
    CHECK(!sel.contains(3));

    sel.clear();
    CHECK(sel.size() == 0);
    CHECK(sel.ids().empty());
}

void test_selection_state_null_sequence() {
    timeline_selection::SelectionState sel;
    sel.set({9}, nullptr);
    CHECK(sel.size() == 1);
    CHECK(sel.contains(9));
    const auto s = make_seq();
    sel.set({1, 3}, &s);
    CHECK(sel.size() == 2);
    CHECK(sel.ids()[0] == 1);
    CHECK(sel.ids()[1] == 3);
}

void test_selection_state_mate_order() {
    const auto s = make_seq();
    timeline_selection::SelectionState sel;
    sel.set({3}, &s);
    CHECK(vec_eq(sel.ids(), {3, 1}));
}

}

int main() {
    test_clips_in_range_basics();
    test_clips_in_range_boundaries();
    test_clips_in_range_empty_sequence();
    test_expand_with_mates();
    test_expand_with_mates_empty_sequence();
    test_selection_state_basics();
    test_selection_state_null_sequence();
    test_selection_state_mate_order();

    if (g_failures == 0) {
        std::printf("timeline_selection_test: ALL PASS\n");
        return 0;
    }
    std::printf("timeline_selection_test: %d FAILURE(S)\n", g_failures);
    return 1;
}
