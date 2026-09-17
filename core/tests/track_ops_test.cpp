#include "canvas/core/timeline/edit_ops.hpp"
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

std::vector<std::string> names(const std::vector<Track>& tracks) {
    std::vector<std::string> out;
    for (const auto& t : tracks) out.push_back(t.name);
    return out;
}

Track video_track(const std::string& name, const MediaId media) {
    Track t;
    t.kind = Track::Kind::Video;
    t.name = name;
    if (media >= 0) {
        Clip c;
        c.id = 1;
        c.media = media;
        c.tl_in = 0;
        c.tl_out = 10;
        c.src_in = 0;
        c.src_out = 10;
        t.clips.push_back(c);
    }
    return t;
}

Sequence three_video_tracks() {
    Sequence s;
    s.fps = 30.0;
    s.video_tracks = {video_track("V1", 11), video_track("V2", 22), video_track("V3", -1)};
    s.audio_tracks = {Track{}, Track{}};
    s.audio_tracks[0].kind = Track::Kind::Audio;
    s.audio_tracks[0].name = "A1";
    s.audio_tracks[1].kind = Track::Kind::Audio;
    s.audio_tracks[1].name = "A2";
    s.next_clip_id = 100;
    return s;
}

void test_insert() {
    Sequence s = three_video_tracks();

    auto cmd = insert_track(s, Track::Kind::Video, 1, "");
    check(cmd != nullptr, "insert middle -> command");
    check(s.video_tracks.size() == 4, "insert middle grows count");
    check(names(s.video_tracks) == std::vector<std::string>({"V1", "V2", "V2", "V3"}),
          "insert middle: default name from position, others shifted");

    cmd->undo(s);
    check(names(s.video_tracks) == std::vector<std::string>({"V1", "V2", "V3"}),
          "insert undo restores order");
    cmd->redo(s);
    check(s.video_tracks.size() == 4, "insert redo re-applies");

    auto tail = insert_track(s, Track::Kind::Video, s.video_tracks.size(), "V9");
    check(tail != nullptr && s.video_tracks.back().name == "V9", "insert at tail with name");
    check(insert_track(s, Track::Kind::Video, s.video_tracks.size() + 1, "") == nullptr,
          "insert past the end rejected");

    auto head = insert_track(s, Track::Kind::Video, 0, "First");
    check(head != nullptr && s.video_tracks.front().name == "First", "insert at head");
}

void test_remove() {
    Sequence s = three_video_tracks();
    auto cmd = remove_track(s, Track::Kind::Video, 1);
    check(cmd != nullptr, "remove middle -> command");
    check(s.video_tracks.size() == 2, "remove shrinks count");
    bool lost_media = false;
    for (const auto& t : s.video_tracks)
        for (const auto& c : t.clips)
            if (c.media == 22) lost_media = true;
    check(!lost_media, "clip on the removed track is gone");

    cmd->undo(s);
    check(s.video_tracks.size() == 3, "remove undo restores count");
    bool restored = false;
    for (const auto& t : s.video_tracks)
        for (const auto& c : t.clips)
            if (c.media == 22) restored = true;
    check(restored, "remove undo restores the removed track's clips");

    check(remove_track(s, Track::Kind::Video, 99) == nullptr, "remove invalid index rejected");

    Sequence one = three_video_tracks();
    one.video_tracks.resize(1);
    check(remove_track(one, Track::Kind::Video, 0) == nullptr, "cannot remove the last video track");
    check(remove_track(one, Track::Kind::Audio, 0) != nullptr,
          "can remove an audio track while another remains");
    check(remove_track(one, Track::Kind::Audio, 0) == nullptr,
          "cannot remove the last audio track");
}

void test_rename() {
    Sequence s = three_video_tracks();

    auto cmd = rename_track(s, Track::Kind::Video, 0, "Hero");
    check(cmd != nullptr && s.video_tracks[0].name == "Hero", "rename applies");
    cmd->undo(s);
    check(s.video_tracks[0].name == "V1", "rename undo restores");
    cmd->redo(s);
    check(s.video_tracks[0].name == "Hero", "rename redo");

    check(rename_track(s, Track::Kind::Video, 99, "x") == nullptr, "rename invalid index rejected");

    auto noop = rename_track(s, Track::Kind::Video, 0, "Hero");
    check(noop != nullptr, "same-name rename returns a (no-op) command");
    check(s.video_tracks[0].name == "Hero", "same-name rename keeps the name");
    noop->undo(s);
    check(s.video_tracks.size() == 3 && s.video_tracks[0].name == "Hero",
          "same-name no-op undo changes nothing");
}

void test_move() {
    Sequence s = three_video_tracks();

    auto cmd = move_track(s, Track::Kind::Video, 0, 2);
    check(cmd != nullptr, "move 0->2 command");
    check(names(s.video_tracks) == std::vector<std::string>({"V2", "V3", "V1"}),
          "move 0->2 reorders");
    check(s.video_tracks[2].clips.size() == 1 && s.video_tracks[2].clips[0].media == 11,
          "clips move with the track");

    cmd->undo(s);
    check(names(s.video_tracks) == std::vector<std::string>({"V1", "V2", "V3"}),
          "move undo restores order");
    cmd->redo(s);
    check(names(s.video_tracks) == std::vector<std::string>({"V2", "V3", "V1"}),
          "move redo");

    auto back = move_track(s, Track::Kind::Video, 2, 0);
    check(back != nullptr &&
              names(s.video_tracks) == std::vector<std::string>({"V1", "V2", "V3"}),
          "move 2->0 reorders back");

    check(move_track(s, Track::Kind::Video, 1, 1) == nullptr, "move from==to rejected");
    check(move_track(s, Track::Kind::Video, 0, 9) == nullptr, "move invalid target rejected");

    auto am = move_track(s, Track::Kind::Audio, 0, 1);
    check(am != nullptr && names(s.audio_tracks) == std::vector<std::string>({"A2", "A1"}),
          "audio move independent of video");
    check(names(s.video_tracks) == std::vector<std::string>({"V1", "V2", "V3"}),
          "video order untouched by audio move");
}

}

int main() {
    test_insert();
    test_remove();
    test_rename();
    test_move();

    if (failures == 0) {
        std::printf("ALL TESTS PASSED\n");
        return 0;
    }
    std::fprintf(stderr, "%d FAILURES\n", failures);
    return 1;
}
