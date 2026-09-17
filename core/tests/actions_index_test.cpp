#include "canvas/core/actions/action_registry.hpp"

#include <cstdio>
#include <string>

using namespace canvas::core::actions;

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

Action make(const char* id, const char* title, std::vector<std::string> keywords = {},
            const char* shortcut = "", const char* category = "") {
    Action a;
    a.id = id;
    a.title = title;
    a.keywords = std::move(keywords);
    a.shortcut = shortcut;
    a.category = category;
    return a;
}

void test_scorer() {
    const Action split = make("split", "Split Clip", {"blade", "cut"}, "Ctrl+B", "Timeline");
    check(score_action(split, "Split Clip") == 1000, "exact title");
    check(score_action(split, "split clip") == 1000, "case-insensitive exact");
    check(score_action(split, "spl") == 800, "title prefix");
    check(score_action(split, "clip") == 600, "title substring");
    check(score_action(split, "bla") == 400, "keyword prefix");
    check(score_action(split, "lade") == 300, "keyword substring");
    check(score_action(split, "timeline") == 150, "category match");
    check(score_action(split, "zzz") == 0, "no match");
}

void test_registry_search() {
    Registry r;
    r.add(make("split", "Split Clip", {"blade"}, "Ctrl+B", "Timeline"));
    r.add(make("ripple", "Ripple Delete", {"remove"}, "", "Timeline"));
    r.add(make("save", "Save Project", {"write"}, "Ctrl+S", "File"));

    check(r.size() == 3, "three actions registered");

    const auto all = r.search("");
    check(all.size() == 3, "empty query returns all");
    check(all[0].action->id == "split" && all[2].action->id == "save",
          "empty query preserves registration order");

    const auto s = r.search("sav");
    check(s.size() == 1 && s[0].action->id == "save", "title prefix finds Save");

    const auto s2 = r.search("BLADE");
    check(s2.size() == 1 && s2[0].action->id == "split", "keyword match is case-insensitive");

    check(r.search("zzz").empty(), "no match -> empty");

    check(r.search("", 2).size() == 2, "limit caps");
}

void test_ranking_and_ties() {
    Registry r;
    r.add(make("a", "Add Marker", {}, "", "Timeline"));
    r.add(make("b", "Marker", {}, "", "Timeline"));
    r.add(make("c", "Clear Markers", {}, "", "Timeline"));

    const auto hits = r.search("marker");
    check(hits.size() == 3, "all three match 'marker'");
    check(hits[0].action->id == "b", "exact title ranks first");
    check(hits[1].action->id == "a" && hits[2].action->id == "c",
          "equal substring scores keep registration order");
}

void test_dedup() {
    Registry r;
    r.add(make("split", "Split Clip"));
    r.add(make("ripple", "Ripple Delete"));
    r.add(make("split", "Split At Playhead", {"blade"}));

    check(r.size() == 2, "duplicate id replaces, no growth");
    check(r.all()[0].id == "split" && r.all()[0].title == "Split At Playhead",
          "replacement updates in place");
    check(r.all()[1].id == "ripple", "other entries keep order");
}

}

int main() {
    test_scorer();
    test_registry_search();
    test_ranking_and_ties();
    test_dedup();

    if (failures == 0) {
        std::printf("ALL TESTS PASSED\n");
        return 0;
    }
    std::fprintf(stderr, "%d FAILURES\n", failures);
    return 1;
}
