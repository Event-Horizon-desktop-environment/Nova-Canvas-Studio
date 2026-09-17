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

std::optional<std::pair<int64_t, int64_t>> win_at(const Project& p, int64_t seq) {
    auto c = TimelineDecoder{}.next_transition_bake_candidate(p, seq);
    if (!c) return std::nullopt;
    return std::make_pair(c->win_start, c->win_end);
}

void test_eligible_same_media_cut() {
    const Project p = make_crossfade_project();
    auto w = win_at(p, 1588);
    report(w.has_value() && w->first == 1678 && w->second == 1679,
           "eligible same-media cut found at 90-frame lead");
    w = win_at(p, 1678);
    report(w.has_value() && w->first == 1678 && w->second == 1679,
           "candidate valid at the window head itself");
    w = win_at(p, 500);
    report(!w.has_value(), "lead beyond kTransitionBakeLead -> no candidate");
}

void test_lead_cap_blocked() {
    Project p = make_crossfade_project();
    const bool blocked = !win_at(p, 1500).has_value();
    report(blocked, "lead of 100 frames exceeds the cap -> blocked");
}

void test_window_longer_than_max() {
    Project p = make_crossfade_project();
    p.sequence.video_tracks[0].clips[0].transition_out_duration = 20;
    report(!win_at(p, 1500).has_value(),
           "window longer than kTransitionBakeMaxFrames -> no candidate");
}

void test_distinct_media_skip() {
    Project p = make_crossfade_project();
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
    p.sequence.video_tracks[0].clips[0].transition_out_duration = 2000;
    report(!win_at(p, 0).has_value(),
           "window overflowing A's own extent -> no candidate");
}

void test_covered_at_window_head() {
    Project p = make_crossfade_project();
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

}

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
