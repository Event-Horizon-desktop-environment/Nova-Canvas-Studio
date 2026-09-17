#include "features/playback/sonicsync.hpp"

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

void test_seek_hold_lifecycle() {
    SonicSync s;
    CHECK(!s.seek_hold_active());
    CHECK(!s.pending_reanchor());

    s.begin_seek_hold(1000);
    CHECK(s.seek_hold_active());
    CHECK(s.pending_reanchor());

    s.begin_seek_hold(2000);
    CHECK(s.seek_hold_active());
    CHECK(s.pending_reanchor());

    s.end_seek_hold();
    CHECK(!s.seek_hold_active());
    CHECK(!s.pending_reanchor());

    SonicSync t;
    t.end_seek_hold();
    CHECK(!t.seek_hold_active());
}

void test_seek_hold_release_drops_flag() {
    SonicSync s;
    s.begin_seek_hold(500);
    CHECK(s.pending_reanchor());
    s.end_seek_hold();
    CHECK(!s.pending_reanchor());
    CHECK(!s.seek_hold_active());
}

void test_on_seek_paused_sets_pending_but_not_hold() {
    SonicSync s;
    s.on_seek_paused(300);
    CHECK(!s.seek_hold_active());
    CHECK(s.pending_reanchor());

    s.confirm_reanchor();
    CHECK(!s.pending_reanchor());

    s.begin_seek_hold(700);
    CHECK(s.seek_hold_active());
    s.on_seek_paused(301);
    CHECK(!s.seek_hold_active());
    CHECK(s.pending_reanchor());
}

void test_reconcile_clamps_to_audible_ceiling() {
    SonicSync s;
    const int64_t want = 100;
    const int64_t aud_seq = 50;
    const int64_t video_lead = 4;
    const int64_t total = 1000;
    const int64_t got = s.reconcile(want, aud_seq, video_lead, total);
    CHECK(got == 54);
}

void test_reconcile_lead_at_least_one() {
    SonicSync s;
    const int64_t want = 100;
    const int64_t aud_seq = 90;
    const int64_t got = s.reconcile(want, aud_seq, 0, 1000);
    CHECK(got == 91);
}

void test_reconcile_no_clamp_when_within_ceiling() {
    SonicSync s;
    const int64_t want = 52;
    const int64_t aud_seq = 50;
    const int64_t got = s.reconcile(want, aud_seq, 4, 1000);
    CHECK(got == want);
}

void test_reconcile_when_aud_seq_behind() {
    SonicSync s;
    const int64_t want = 40;
    const int64_t aud_seq = 200;
    const int64_t got = s.reconcile(want, aud_seq, 4, 1000);
    CHECK(got == want);
}

void test_reconcile_when_aud_seq_unknown() {
    SonicSync s;
    const int64_t want = 900;
    const int64_t got = s.reconcile(want, -1, 4, 1000);
    CHECK(got == want);
}

void test_reconcile_clamps_to_bounds() {
    SonicSync s;
    CHECK(s.reconcile(2000, -1, 4, 100) == 99);
    CHECK(s.reconcile(-5, -1, 4, 100) == 0);
    CHECK(s.reconcile(50, 10, 4, 0) == 50);
}

}

int main() {
    test_seek_hold_lifecycle();
    test_seek_hold_release_drops_flag();
    test_on_seek_paused_sets_pending_but_not_hold();
    test_reconcile_clamps_to_audible_ceiling();
    test_reconcile_lead_at_least_one();
    test_reconcile_no_clamp_when_within_ceiling();
    test_reconcile_when_aud_seq_behind();
    test_reconcile_when_aud_seq_unknown();
    test_reconcile_clamps_to_bounds();

    if (g_failures == 0) {
        std::printf("sonicsync_test: ALL PASS\n");
        return 0;
    }
    std::printf("sonicsync_test: %d FAILURE(S)\n", g_failures);
    return 1;
}
