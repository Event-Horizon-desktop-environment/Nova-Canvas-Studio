#include "canvas/core/timeline/caption_burn.hpp"
#include "canvas/core/timeline/captions.hpp"

#include <cstdio>
#include <string>
#include <vector>

using namespace canvas::core;
using namespace canvas::core::caption_burn;

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

captions::Caption cap(const int64_t s, const int64_t e, const char* text) {
    captions::Caption c;
    c.start_ms = s;
    c.end_ms = e;
    c.text = text;
    return c;
}

void test_windows_from() {
    const auto w = windows_from({cap(0, 1000, "a"), cap(500, 1000, "b")}, 30.0);
    check(w.size() == 2, "two windows");
    check(w[0].start == 0 && w[0].end == 30, "0..1000ms -> 0..30 frames");
    check(w[1].start == 15 && w[1].end == 30, "500ms -> frame 15");

    const auto r = windows_from({cap(33, 100, "x")}, 30.0);
    check(r[0].start == 1, "33ms rounds to frame 1");

    const auto fb = windows_from({cap(0, 1000, "x")}, 0.0);
    check(fb[0].end == 30, "fps<=0 falls back to 30");

    const auto deg = windows_from({cap(1000, 0, "x")}, 30.0);
    check(deg[0].end >= deg[0].start, "degenerate window not inverted");
}

void test_ownership() {
    const std::vector<Window> w = {{10, 20, "only"}};
    check(active_at(w, 9) == -1, "before start -> none");
    check(active_at(w, 10) == 0, "start is inclusive");
    check(active_at(w, 19) == 0, "last covered frame");
    check(active_at(w, 20) == -1, "end is exclusive");
    check(active_at(w, 21) == -1, "past end -> none");
}

void test_overlap_later_wins() {
    const std::vector<Window> w = {{0, 10, "early"}, {5, 15, "late"}};
    check(active_at(w, 3) == 0, "only early covers frame 3");
    check(active_at(w, 7) == 1, "overlap: later caption wins");
    check(active_at(w, 12) == 1, "only late covers frame 12");
    check(text_at(w, 7) == "late", "text_at returns the winning caption");
    check(text_at(w, 30).empty(), "text_at empty when none");
}

void test_tie_breaks() {
    const std::vector<Window> a = {{0, 5, "short"}, {0, 9, "long"}};
    check(active_at(a, 3) == 1, "same start -> greater end wins");

    const std::vector<Window> b = {{0, 5, "first"}, {0, 5, "second"}};
    check(active_at(b, 2) == 1, "identical -> later index wins");
}

void test_empty() {
    check(active_at({}, 0) == -1, "empty -> -1");
    check(text_at({}, 5).empty(), "empty -> empty text");
}

}

int main() {
    test_windows_from();
    test_ownership();
    test_overlap_later_wins();
    test_tie_breaks();
    test_empty();

    if (failures == 0) {
        std::printf("ALL TESTS PASSED\n");
        return 0;
    }
    std::fprintf(stderr, "%d FAILURES\n", failures);
    return 1;
}
