#include "canvas/core/export/chapters.hpp"
#include "canvas/core/export/exporter.hpp"
#include "canvas/core/timeline/model.hpp"

#include <cstdio>
#include <string>

using namespace canvas::core;
using namespace canvas::core::chapters;

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

Sequence marked_sequence() {
    Sequence seq;
    seq.fps = 30.0;
    Track t;
    t.kind = Track::Kind::Video;
    Clip c;
    c.id = 1;
    c.tl_in = 0;
    c.tl_out = 900;
    t.clips.push_back(c);
    seq.video_tracks.push_back(t);
    (void)seq.toggle_bookmark(90, "Intro");
    (void)seq.toggle_bookmark(300, "Body");
    (void)seq.toggle_bookmark(600, "");
    return seq;
}

void test_capability() {
    check(container_supports("mp4"), "mp4 carries chapters");
    check(container_supports("mov"), "mov carries chapters");
    check(container_supports("mkv"), "mkv carries chapters");
    check(container_supports("matroska"), "matroska carries chapters");
    check(container_supports("webm"), "webm carries chapters");
    check(container_supports("MP4"), "capability is case-insensitive");
    check(!container_supports("avi"), "avi does not carry chapters");
    check(!container_supports("mxf"), "mxf does not carry chapters");
    check(!container_supports("image2"), "image2 does not carry chapters");
    check(!container_supports(""), "empty format does not carry chapters");
}

void test_for_export() {
    const Sequence seq = marked_sequence();

    check(for_export(seq, false).empty(), "disabled -> empty chapter table");

    const auto chapters = for_export(seq, true);
    check(chapters.size() == 3, "one chapter per marker");
    if (chapters.size() == 3) {
        check(chapters[0].start_seconds == 3.0, "first start = 3s");
        check(chapters[0].end_seconds == 10.0, "first end = next start (10s)");
        check(chapters[0].title == "Intro", "first title from marker");
        check(chapters[1].start_seconds == 10.0 && chapters[1].end_seconds == 20.0,
              "middle chapter spans to next");
        check(chapters[2].start_seconds == 20.0, "last start = 20s");
        check(chapters[2].end_seconds == 30.0, "last end = sequence duration");
        check(chapters[2].title == "Chapter 3", "empty label -> Chapter N");
    }

    Sequence bare;
    bare.fps = 30.0;
    check(for_export(bare, true).empty(), "no markers -> empty table");
}

void test_apply() {
    const Sequence seq = marked_sequence();

    ExportSettings es;
    es.format = "mp4";
    apply(es, seq, true);
    check(es.chapters.size() == 3, "apply installs chapters for a capable format");

    es.chapters.clear();
    es.format = "avi";
    apply(es, seq, true);
    check(es.chapters.empty(), "apply skips an incapable format");

    es.format = "mp4";
    apply(es, seq, false);
    check(es.chapters.empty(), "apply clears when disabled");

    apply(es, seq, true);
    check(es.chapters.size() == 3, "re-apply repopulates");
}

}

int main() {
    test_capability();
    test_for_export();
    test_apply();

    if (failures == 0) {
        std::printf("ALL TESTS PASSED\n");
        return 0;
    }
    std::fprintf(stderr, "%d FAILURES\n", failures);
    return 1;
}
