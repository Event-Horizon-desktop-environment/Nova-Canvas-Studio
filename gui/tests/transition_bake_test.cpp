// Qt-free unit test for TimelineDecoder's off-thread transition pre-render
// DISCOVERY (next_transition_bake_candidate). Links ONLY canvas_core +
// timeline_decoder.cpp (no Qt) — the same "unbreakable seam" guarantee as
// timeline_decoder_test.
//
// Deliberately pure: the candidate scan is the single unit that can be
// exercised headlessly (the thread/GPU/decode half of the bake needs a CUDA
// device and a running timeline). The shape under test is the exact reported
// freeze: a 1-frame cross-dissolve between two hits of the SAME 60fps media on
// a 30fps timeline, where the incoming clip's source starts 90 media frames
// ahead of A's tail.
//
// Covers: the eligible same-media cut (lead within kTransitionBakeLead), lead
// beyond the cap, window longer than kTransitionBakeMaxFrames, distinct-media
// cut, missing incoming clip, single-clip fade only, audio-only transition,
// window overflowing clip A's own extent, a higher track covering the window
// head, and a disabled candidate clip.

#include "features/playback/timeline_decoder.hpp"

#include "canvas/core/project/project.hpp"
#include "canvas/core/timeline/model.hpp"

#include <cstdint>
#include <cstdio>
#include <optional>

using namespace canvas::core;
using canvas::gui::TimelineDecoder;

static int g_failures = 0;

static void report(bool ok, const char* what) {
    std::printf("%s  %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++g_failures;
}

namespace {

// The reported freeze: 1-frame cross-dissolve at tl 1678 between A (tl
// [0,1679), src [0,3358]) and B (tl [1679,24145), src [3448,48380]) — B's head
// sits 90 media frames past A's tail on the SAME file, so a live walk costs two
// far keyframe climbs.
Project make_crossfade_project() {
    Project p;
    p.sequence.fps = 30.0;
    MediaEntry m;
    m.id = 0;
    m.path = "/tmp/canvas_bake_test.mkv";
    m.fps = 60.0;
    m.width = 2560;
    m.height = 1440;
    m.total_frames = 48380;
    p.media.push_back(m);

    Track t;
    Clip a;
    a.id = 1;
    a.media = 0;
    a.tl_in = 0;
    a.tl_out = 1679;
    a.src_in = 0;
    a.src_out = 3358;
    a.transition_out = TransitionType::CrossDissolve;
    a.transition_out_duration = 1;
    Clip b;
    b.id = 2;
    b.media = 0;
    b.tl_in = 1679;
    b.tl_out = 24145;
    b.src_in = 3448;
    b.src_out = 48380;
    t.clips = {a, b};
    p.sequence.video_tracks = {t};
    return p;
}

// Extracts just the win bounds for readable asserts.
std::optional<std::pair<int64_t, int64_t>> win_at(const Project& p, int64_t seq) {
    auto c = TimelineDecoder{}.next_transition_bake_candidate(p, seq);
    if (!c) return std::nullopt;
    return std::make_pair(c->win_start, c->win_end);
}

void test_eligible_same_media_cut() {
    const Project p = make_crossfade_project();
    // 90 TL frames ahead of the window head = inside the 96-frame lead.
    auto w = win_at(p, 1588);
    report(w.has_value() && w->first == 1678 && w->second == 1679,
           "eligible same-media cut found at 90-frame lead");
    // Playhead already AT the window head still qualifies (lead 0).
    w = win_at(p, 1678);
    report(w.has_value() && w->first == 1678 && w->second == 1679,
           "candidate valid at the window head itself");
    // Far outside the lead cap: no bake, live path handles it.
    w = win_at(p, 500);
    report(!w.has_value(), "lead beyond kTransitionBakeLead -> no candidate");
}

void test_lead_cap_blocked() {
    Project p = make_crossfade_project();
    // 100-frame lead is past the 96-frame cap.
    const bool blocked = !win_at(p, 1500).has_value();
    report(blocked, "lead of 100 frames exceeds the cap -> blocked");
}

void test_window_longer_than_max() {
    Project p = make_crossfade_project();
    // 20-frame window > kTransitionBakeMaxFrames (16): keep the live path.
    p.sequence.video_tracks[0].clips[0].transition_out_duration = 20;
    report(!win_at(p, 1500).has_value(),
           "window longer than kTransitionBakeMaxFrames -> no candidate");
}

void test_distinct_media_skip() {
    Project p = make_crossfade_project();
    // B references a second media entry: the far double-walk no longer applies.
    MediaEntry m2;
    m2.id = 1;
    m2.path = "/tmp/canvas_bake_test_other.mkv";
    m2.fps = 60.0;
    p.media.push_back(m2);
    p.sequence.video_tracks[0].clips[1].media = 1;
    report(!win_at(p, 1588).has_value(), "distinct-media cut -> no candidate");
}

void test_missing_incoming_clip() {
    Project p = make_crossfade_project();
    p.sequence.video_tracks[0].clips.pop_back();
    report(!win_at(p, 1588).has_value(),
           "no incoming clip at the cut -> fade stays on the live path");
}

void test_fade_only_no_out() {
    Project p = make_crossfade_project();
    p.sequence.video_tracks[0].clips[0].transition_out = TransitionType::None;
    p.sequence.video_tracks[0].clips[0].transition_out_duration = 0;
    report(!win_at(p, 1588).has_value(), "no OUT transition -> no candidate");
}

void test_audio_only_transition_skip() {
    Project p = make_crossfade_project();
    p.sequence.video_tracks[0].clips[0].transition_out = TransitionType::AudioFadeConstantGain;
    report(!win_at(p, 1588).has_value(),
           "audio-only OUT transition -> no candidate (video window unchanged)");
}

void test_window_overflowing_clip() {
    Project p = make_crossfade_project();
    // Make the window reach before A's own tl_in: no valid window exists.
    p.sequence.video_tracks[0].clips[0].transition_out_duration = 2000;
    report(!win_at(p, 0).has_value(),
           "window overflowing A's own extent -> no candidate");
}

void test_covered_at_window_head() {
    Project p = make_crossfade_project();
    // A higher track paints over win_start=1678: at present time it wins the
    // composite, so baking A would be wasted (and worse — the overlay would
    // disappear mid-window).
    Track top;
    Clip c;
    c.id = 3;
    c.media = 0;
    c.tl_in = 1600;
    c.tl_out = 1700;
    c.src_in = 3200;
    c.src_out = 3300;
    top.clips.push_back(c);
    p.sequence.video_tracks.push_back(top);
    report(!win_at(p, 1588).has_value(), "higher track at window head -> no candidate");
}

void test_disabled_candidate() {
    Project p = make_crossfade_project();
    p.sequence.video_tracks[0].clips[0].enabled = false;
    report(!win_at(p, 1588).has_value(), "disabled candidate clip -> no candidate");
}

}  // namespace

int main() {
    test_eligible_same_media_cut();
    test_lead_cap_blocked();
    test_window_longer_than_max();
    test_distinct_media_skip();
    test_missing_incoming_clip();
    test_fade_only_no_out();
    test_audio_only_transition_skip();
    test_window_overflowing_clip();
    test_covered_at_window_head();
    test_disabled_candidate();
    std::printf("%s\n", g_failures == 0 ? "transition_bake_test: ALL PASS"
                                        : "transition_bake_test: FAILURES");
    return g_failures == 0 ? 0 : 1;
}