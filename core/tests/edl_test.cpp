#include "canvas/core/export/edl.hpp"
#include "canvas/core/project/project.hpp"
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

bool has(const std::string& hay, const std::string& needle) {
    return hay.find(needle) != std::string::npos;
}

void test_timecode() {
    check(edl::timecode(0, 30) == "00:00:00:00", "frame 0");
    check(edl::timecode(29, 30) == "00:00:00:29", "frame 29");
    check(edl::timecode(30, 30) == "00:00:01:00", "frame 30 = 1s");
    check(edl::timecode(90, 30) == "00:00:03:00", "frame 90 = 3s");
    check(edl::timecode(108000, 30) == "01:00:00:00", "1 hour at 30fps");
    check(edl::timecode(-5, 30) == "00:00:00:00", "negative clamps to 0");
    check(edl::timecode(25, 25) == "00:00:01:00", "25fps boundary");
    check(edl::timecode(25, 0) == "00:00:00:25", "fps<=0 falls back to 30");
}

Clip make_clip(const MediaId media, const int64_t tl_in, const int64_t tl_out,
               const int64_t src_in, const int64_t src_out, const ClipId id) {
    Clip c;
    c.id = id;
    c.media = media;
    c.tl_in = tl_in;
    c.tl_out = tl_out;
    c.src_in = src_in;
    c.src_out = src_out;
    return c;
}

void test_write() {
    Project p;
    p.sequence.fps = 30.0;

    Track v1;
    v1.kind = Track::Kind::Video;
    v1.name = "V1";
    v1.clips.push_back(make_clip(1, 0, 150, 0, 150, 1));
    Track v2;
    v2.kind = Track::Kind::Video;
    v2.name = "V2";
    v2.clips.push_back(make_clip(2, 30, 90, 0, 60, 2));
    Track a1;
    a1.kind = Track::Kind::Audio;
    a1.name = "A1";
    a1.clips.push_back(make_clip(3, 0, 150, 0, 150, 3));
    p.sequence.video_tracks = {v1, v2};
    p.sequence.audio_tracks = {a1};

    MediaEntry m1;
    m1.id = 1;
    m1.path = "/media/foo.mov";
    MediaEntry m2;
    m2.id = 2;
    m2.path = "/media/baz.mp4";
    MediaEntry m3;
    m3.id = 3;
    m3.path = "/media/bar.wav";
    p.media = {m1, m2, m3};

    const std::string edl = edl::write_cmx3600(p.sequence, "Demo", p.media);

    check(has(edl, "TITLE: Demo\n"), "TITLE line");
    check(has(edl, "FCM: NON-DROP FRAME\n\n"), "FCM line");

    check(has(edl, "001"), "event 001 present");
    check(has(edl, "FOO"), "reel from foo.mov");
    check(has(edl, "00:00:00:00 00:00:05:00 00:00:00:00 00:00:05:00"),
          "event 001 src + rec timecodes");
    check(has(edl, "* FROM CLIP NAME: foo.mov\n"), "clip-name note");

    const std::size_t pos_bar = edl.find("BAR");
    const std::size_t pos_baz = edl.find("BAZ");
    check(pos_bar != std::string::npos && pos_baz != std::string::npos, "BAR + BAZ reels");
    check(pos_bar < pos_baz, "audio event (BAR) before later video event (BAZ)");

    check(has(edl, "00:00:00:00 00:00:02:00 00:00:01:00 00:00:03:00"),
          "event 003 src + rec timecodes");

    Sequence t;
    t.fps = 30.0;
    Track tv;
    tv.kind = Track::Kind::Video;
    tv.clips.push_back(make_clip(-1, 0, 30, 0, 30, 1));
    t.video_tracks.push_back(tv);
    const std::string tedl = edl::write_cmx3600(t, "Titles", {});
    check(has(tedl, "AX"), "title clip -> AX reel");
    check(!has(tedl, "FROM CLIP NAME"), "title clip has no media note");
}

}

int main() {
    test_timecode();
    test_write();

    if (failures == 0) {
        std::printf("ALL TESTS PASSED\n");
        return 0;
    }
    std::fprintf(stderr, "%d FAILURES\n", failures);
    return 1;
}
