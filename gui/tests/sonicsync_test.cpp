// Headless SonicSync unit tests (Phase 18). Compiles sonicsync.cpp directly
// into the binary (like timeline_decoder_test / audio_pipeline_test) so the
// module is verified exactly as shipped; the Qt-free seam is enforced by the
// build (a stray <Q...> include breaks this target on purpose).
//
// Covers the four behaviours SonicSync owns:
//   * seek-hold begin/end lifecycle (audio rides with its frame);
//   * paused-seek re-anchor flag (audio irrelevant, just note the move);
//   * reconcile clamps video above the audible ceiling (master clock);
//   * reconcile passes-through / never clamped when aud_seq is unknown/at/behind.

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
    // Fresh state: no hold, no pending re-anchor.
    CHECK(!s.seek_hold_active());
    CHECK(!s.pending_reanchor());

    // Begin a seek while playing -> hold engages, re-anchor flagged.
    s.begin_seek_hold(1000);
    CHECK(s.seek_hold_active());
    CHECK(s.pending_reanchor());

    // A second begin (caller re-targeting before present) stays held.
    s.begin_seek_hold(2000);
    CHECK(s.seek_hold_active());
    CHECK(s.pending_reanchor());

    // Presenting the target frame releases the hold and clears the re-anchor.
    s.end_seek_hold();
    CHECK(!s.seek_hold_active());
    CHECK(!s.pending_reanchor());

    // end_seek_hold when not in a hold (paused path / spurious) is a no-op.
    SonicSync t;
    t.end_seek_hold();
    CHECK(!t.seek_hold_active());
}

void test_seek_hold_release_drops_flag() {
    SonicSync s;
    s.begin_seek_hold(500);
    CHECK(s.pending_reanchor());
    // Releasing the hold is the only thing that clears the re-anchor flag.
    s.end_seek_hold();
    CHECK(!s.pending_reanchor());
    CHECK(!s.seek_hold_active());
}

void test_on_seek_paused_sets_pending_but_not_hold() {
    SonicSync s;
    // Paused scrub/transport: no audio to hold, but the position moved.
    s.on_seek_paused(300);
    CHECK(!s.seek_hold_active());
    CHECK(s.pending_reanchor());

    // A later play must re-anchor: confirm() is the caller's acknowledgement.
    s.confirm_reanchor();
    CHECK(!s.pending_reanchor());

    // on_seek_paused also clears a stale hold from a previous play.
    s.begin_seek_hold(700);
    CHECK(s.seek_hold_active());
    s.on_seek_paused(301);
    CHECK(!s.seek_hold_active());
    CHECK(s.pending_reanchor());
}

void test_reconcile_clamps_to_audible_ceiling() {
    SonicSync s;
    // aud_seq known, video attempts to run far past the audible ceiling.
    const int64_t want = 100;
    const int64_t aud_seq = 50;
    const int64_t video_lead = 4;   // history: video may sit up to 4 past audible
    const int64_t total = 1000;
    const int64_t got = s.reconcile(want, aud_seq, video_lead, total);
    CHECK(got == 54);               // aud_seq + video_lead == ceiling
}

void test_reconcile_lead_at_least_one() {
    SonicSync s;
    // Even with video_lead == 0 the ceiling is audible + at least 1 frame.
    const int64_t want = 100;
    const int64_t aud_seq = 90;
    const int64_t got = s.reconcile(want, aud_seq, 0, 1000);
    CHECK(got == 91);
}

void test_reconcile_no_clamp_when_within_ceiling() {
    SonicSync s;
    // want is comfortably inside the audible ceiling -> passes through.
    const int64_t want = 52;
    const int64_t aud_seq = 50;
    const int64_t got = s.reconcile(want, aud_seq, 4, 1000);
    CHECK(got == want);
}

void test_reconcile_when_aud_seq_behind() {
    SonicSync s;
    // Picture behind audible -> just present `want` (never stall).
    const int64_t want = 40;
    const int64_t aud_seq = 200;
    const int64_t got = s.reconcile(want, aud_seq, 4, 1000);
    CHECK(got == want);
}

void test_reconcile_when_aud_seq_unknown() {
    SonicSync s;
    // aud_seq == -1 (unknown): no ceiling applied; want clamps only to [0,total).
    const int64_t want = 900;
    const int64_t got = s.reconcile(want, -1, 4, 1000);
    CHECK(got == want);
}

void test_reconcile_clamps_to_bounds() {
    SonicSync s;
    // Unknown audible position, want past the end -> clamped to total-1.
    CHECK(s.reconcile(2000, -1, 4, 100) == 99);
    // Negative want -> clamped to 0.
    CHECK(s.reconcile(-5, -1, 4, 100) == 0);
    // Total <= 0 -> returns want unchanged (no sequence, no clamp).
    CHECK(s.reconcile(50, 10, 4, 0) == 50);
}

}  // namespace

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