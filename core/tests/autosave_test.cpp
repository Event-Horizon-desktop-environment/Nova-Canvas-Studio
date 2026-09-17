#include "canvas/core/project/autosave.hpp"
#include "canvas/core/project/project.hpp"

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

using namespace canvas::core;
using namespace canvas::core::autosave;

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

bool has_slot(const std::vector<int>& v, const int s) {
    return std::find(v.begin(), v.end(), s) != v.end();
}

void test_naming() {
    check(snapshot_path("/p/foo.ncs", 3) == "/p/foo.ncs.autosave-3", "snapshot_path slot 3");
    check(snapshot_path("/p/foo.ncs", 0) == "/p/foo.ncs.autosave-1", "slot clamps to >= 1");

    check(slot_from_snapshot("/p/foo.ncs", "/p/foo.ncs.autosave-4") == 4, "slot parsed");
    check(slot_from_snapshot("/p/foo.ncs", "/p/bar.ncs.autosave-4") == 0, "wrong project -> 0");
    check(slot_from_snapshot("/p/foo.ncs", "/p/foo.ncs.autosave-x") == 0, "non-numeric -> 0");
    check(slot_from_snapshot("/p/foo.ncs", "/p/foo.ncs") == 0, "not a snapshot -> 0");
    check(slot_from_snapshot("/p/foo.ncs", "/p/foo.ncs.autosave-") == 0, "empty slot -> 0");
}

void test_recovery() {
    Policy p;
    p.max_snapshots = 5;
    check(newest_slot({}, p) == 0, "no snapshots -> 0");
    check(newest_slot({2, 5, 1}, p) == 5, "newest is the highest valid slot");
    check(newest_slot({7, 0, -3}, p) == 0, "out-of-range slots ignored");
    check(newest_slot({3, 9}, p) == 3, "mixed: highest valid wins");
}

void test_turnover() {
    Policy p;
    p.max_snapshots = 5;

    {
        const Turnover t = plan_turnover({}, p);
        check(t.write_slot == 1 && t.remove_slots.empty(), "empty -> write 1");
    }
    {
        const Turnover t = plan_turnover({1, 2}, p);
        check(t.write_slot == 3 && t.remove_slots.empty(), "lowest free slot");
    }
    {
        const Turnover t = plan_turnover({1, 3}, p);
        check(t.write_slot == 2 && t.remove_slots.empty(), "fills the gap");
    }
    {
        const Turnover t = plan_turnover({1, 2, 3, 4, 5}, p);
        check(t.write_slot == 1 && t.remove_slots.empty(), "full ring -> overwrite successor");
    }
    {
        const Turnover t = plan_turnover({1, 2, 3, 4, 5, 6, 7}, p);
        check(t.write_slot == 1, "over-cap ring still writes in range");
        check(t.remove_slots.size() == 2 && has_slot(t.remove_slots, 6) &&
                  has_slot(t.remove_slots, 7),
              "over-cap slots rolled off");
    }
    {
        const Turnover t = plan_turnover({2, 2, 3}, p);
        check(t.write_slot == 1 && t.remove_slots.empty(), "duplicates collapse");
    }
    {
        Policy one;
        one.max_snapshots = 0;
        const Turnover t = plan_turnover({1}, one);
        check(t.write_slot == 1, "single-slot policy clamps to 1");
    }
}

void test_manifest() {
    Project proj;
    proj.name = "Doc";
    MediaEntry a;
    a.id = 1;
    a.width = 1920;
    a.height = 1080;
    a.path = "/media/a.mov";
    MediaEntry b;
    b.id = 2;
    b.width = 1280;
    b.height = 720;
    b.path = "/media/b.mp4";
    proj.media = {a, b};

    const std::string man = media_manifest(proj);
    check(man.find("Project: Doc") != std::string::npos, "manifest has project name");
    check(man.find("/media/a.mov") != std::string::npos, "manifest lists a.mov");
    check(man.find("/media/b.mp4") != std::string::npos, "manifest lists b.mp4");
    check(man.find("1920x1080") != std::string::npos, "manifest includes dimensions");
}

}

int main() {
    test_naming();
    test_recovery();
    test_turnover();
    test_manifest();

    if (failures == 0) {
        std::printf("ALL TESTS PASSED\n");
        return 0;
    }
    std::fprintf(stderr, "%d FAILURES\n", failures);
    return 1;
}
