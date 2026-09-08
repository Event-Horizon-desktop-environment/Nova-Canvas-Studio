// Headless audio-target resolution tests (Phase 4). Compiles
// audio_targets.cpp directly into the binary so the module is verified exactly
// as shipped; the Qt-free seam is enforced by the build (a stray <Q...> include
// breaks this target on purpose).
//
// Covers the mixer-target resolution the Inspector's Volume/Pan edits drive:
//   * audio clip in the selection resolves to itself (kind/track/id copied);
//   * video clip with a linked audio mate resolves to that mate;
//   * video clip with no audio is skipped;
//   * a linked A/V pair selected via either half yields ONE target (de-dup);
//   * duplicate ids in the selection collapse;
//   * order follows first occurrence in the selection;
//   * the full Clip copy rides along (volume_db/pan readable for populate).

#include "features/timeline/audio_targets.hpp"

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

canvas::core::Sequence make_seq() {
    canvas::core::Sequence s;
    canvas::core::Track v0;
    v0.kind = canvas::core::Track::Kind::Video;
    v0.clips = {
        {.id = 1, .tl_in = 0, .tl_out = 100, .linked_id = 4},     // V+A pair (mate A0)
        {.id = 2, .tl_in = 100, .tl_out = 160},                   // video, NO audio
        {.id = 7, .tl_in = 400, .tl_out = 500, .linked_id = 8},   // V+A pair (mate A1)
    };
    canvas::core::Track v1;
    v1.kind = canvas::core::Track::Kind::Video;
    v1.clips = {
        {.id = 3, .tl_in = 200, .tl_out = 300},                   // pure video
    };
    canvas::core::Track a0;
    a0.kind = canvas::core::Track::Kind::Audio;
    a0.clips = {
        {.id = 4, .tl_in = 0, .tl_out = 100, .linked_id = 1},
        {.id = 5, .tl_in = 100, .tl_out = 200, .volume_db = -6.0f, .pan = 0.5f},  // audio only
    };
    canvas::core::Track a1;
    a1.kind = canvas::core::Track::Kind::Audio;
    a1.clips = {
        {.id = 8, .tl_in = 400, .tl_out = 500, .linked_id = 7},
    };
    s.video_tracks.push_back(v0);
    s.video_tracks.push_back(v1);
    s.audio_tracks.push_back(a0);
    s.audio_tracks.push_back(a1);
    return s;
}

bool any_target(const std::vector<AudioTarget>& ts, canvas::core::ClipId id,
                std::size_t track) {
    for (const auto& t : ts)
        if (t.id == id && t.track == track) return true;
    return false;
}

void test_audio_clip_direct() {
    const auto s = make_seq();
    const auto ts = resolve_audio_targets(s, {5});
    CHECK(ts.size() == 1);
    if (ts.size() == 1) {
        CHECK(ts[0].kind == canvas::core::Track::Kind::Audio);
        CHECK(ts[0].track == 0);
        CHECK(ts[0].id == 5);
        CHECK(ts[0].clip.id == 5);
        CHECK(ts[0].clip.volume_db == -6.0f);
        CHECK(ts[0].clip.pan == 0.5f);
    }
}

void test_video_mate_resolves() {
    const auto s = make_seq();
    const auto ts = resolve_audio_targets(s, {1});
    CHECK(ts.size() == 1);
    if (ts.size() == 1) {
        CHECK(ts[0].id == 4);
        CHECK(ts[0].track == 0);
    }
}

void test_video_mate_on_second_audio_track() {
    const auto s = make_seq();
    const auto ts = resolve_audio_targets(s, {7});
    CHECK(ts.size() == 1);
    if (ts.size() == 1) {
        CHECK(ts[0].id == 8);
        CHECK(ts[0].track == 1);
    }
}

void test_video_without_audio_skipped() {
    const auto s = make_seq();
    CHECK(resolve_audio_targets(s, {2}).empty());
    CHECK(resolve_audio_targets(s, {3}).empty());
}

void test_linked_pair_selected_via_either_half() {
    const auto s = make_seq();
    // Video half alone and audio half alone each resolve to the same single
    // audio target...
    const auto tv = resolve_audio_targets(s, {1});
    const auto ta = resolve_audio_targets(s, {4});
    CHECK(tv.size() == 1 && tv[0].id == 4);
    CHECK(ta.size() == 1 && ta[0].id == 4);
    // ...and selecting BOTH halves still yields ONE target (no double edit).
    const auto both = resolve_audio_targets(s, {1, 4});
    CHECK(both.size() == 1 && both[0].id == 4);
    const auto both_rev = resolve_audio_targets(s, {4, 1});
    CHECK(both_rev.size() == 1 && both_rev[0].id == 4);
}

void test_multi_selection_in_order_and_dedupe() {
    const auto s = make_seq();
    // {video-with-mate, audio-only, pure-video, duplicate} -> {4, 5}, 3 skipped.
    const auto ts = resolve_audio_targets(s, {1, 5, 3, 5, 4});
    CHECK(ts.size() == 2);
    if (ts.size() == 2) {
        CHECK(ts[0].id == 4 && ts[0].track == 0);
        CHECK(ts[1].id == 5 && ts[1].track == 0);
    }
}

void test_pair_split_across_tracks() {
    const auto s = make_seq();
    const auto ts = resolve_audio_targets(s, {3, 8, 2});
    CHECK(ts.size() == 1);
    if (ts.size() == 1) {
        CHECK(ts[0].id == 8 && ts[0].track == 1);
    }
}

void test_empty_selection() {
    const auto s = make_seq();
    CHECK(resolve_audio_targets(s, {}).empty());
    CHECK(resolve_audio_targets(s, {2, 3}).empty());
}

}  // namespace

int main() {
    test_audio_clip_direct();
    test_video_mate_resolves();
    test_video_mate_on_second_audio_track();
    test_video_without_audio_skipped();
    test_linked_pair_selected_via_either_half();
    test_multi_selection_in_order_and_dedupe();
    test_pair_split_across_tracks();
    test_empty_selection();

    if (g_failures == 0) {
        std::printf("audio_targets_test: ALL PASS\n");
        return 0;
    }
    std::printf("audio_targets_test: %d FAILURE(S)\n", g_failures);
    return 1;
}