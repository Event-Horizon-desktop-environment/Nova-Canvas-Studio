#include "canvas/core/media/transcript.hpp"

#include <cstdio>
#include <fstream>
#include <string>

namespace {

int g_failures = 0;

void report(const bool ok, const char* name) {
    std::printf("%s  %s\n", ok ? "PASS" : "FAIL", name);
    if (!ok) ++g_failures;
}

}

int main() {
    using canvas::core::transcript::Segment;
    using canvas::core::transcript::clean_segment_text;
    using canvas::core::transcript::srt_join;
    using canvas::core::transcript::srt_timestamp;
    using canvas::core::transcript::write_srt;

    report(srt_timestamp(0) == "00:00:00,000", "time: zero -> 00:00:00,000");
    report(srt_timestamp(65'123) == "00:01:05,123", "time: 65s123ms -> 00:01:05,123");
    report(srt_timestamp(3'600'000 + 1'000) == "01:00:01,000", "time: 1h1s -> 01:00:01,000");
    report(srt_timestamp(99 * 3600'000 + 59 * 60'000 + 59'000 + 999) == "99:59:59,999",
           "time: 99:59:59.999 exact max");
    report(srt_timestamp(-1) == "00:00:00,000", "time: negative clamps to zero");
    report(srt_timestamp(100 * 3600'000) == "99:59:59,999", "time: >=100h clamps to max");

    report(clean_segment_text("  Hello   world \t here  ") == "Hello world here",
           "text: interior whitespace collapses, edges trimmed");
    report(clean_segment_text("one\ntwo\r\nthree") == "one two three",
           "text: newlines collapse to spaces");
    report(clean_segment_text("") == "", "text: empty stays empty");
    report(clean_segment_text("   \t\n ") == "", "text: whitespace-only -> empty");
    report(clean_segment_text("word") == "word", "text: bare word unchanged");

    {
        const std::vector<Segment> segs = {
            {2'000, 5'000, "  Hello   world"},
            {5'500, 6'000, "   "},
            {6'100, 6'250, "Done"},
        };
        const std::string want = "1\r\n"
                                 "00:00:02,000 --> 00:00:05,000\r\n"
                                 "Hello world\r\n"
                                 "\r\n"
                                 "2\r\n"
                                 "00:00:06,100 --> 00:00:06,250\r\n"
                                 "Done\r\n"
                                 "\r\n";
        report(srt_join(segs) == want, "join: exact SRT text (skip silent, 1-based cues)");
    }
    report(srt_join({}) == "", "join: empty segment list -> empty SRT");

    const std::string path = "/tmp/opencode/media/transcript.srt";
    if (std::FILE* probe = std::fopen("/tmp/opencode/media/probe", "wb")) {
        std::fclose(probe);
        std::remove("/tmp/opencode/media/probe");

        const std::vector<Segment> segs = {{1'000, 3'500,
                                             "Caf\xC3\xA9 d\xC3\xA9j\xC3\xA0 vu"}};
        report(write_srt(path, segs), "writer: returns true on success");

        std::ifstream in(path, std::ios::binary);
        bool same = false;
        if (in) {
            const std::string got((std::istreambuf_iterator<char>(in)),
                                  std::istreambuf_iterator<char>());
            same = got == "1\r\n00:00:01,000 --> 00:00:03,500\r\nCaf\xC3\xA9 d\xC3\xA9j\xC3\xA0 vu\r\n\r\n";
            std::printf("      file bytes=%zu\n", got.size());
        }
        report(same, "writer: file round-trips byte-exact (UTF-8)");
        std::remove(path.c_str());
    } else {
        std::printf("SKIP writer tests (no /tmp/opencode/media)\n");
    }

    const bool ok = g_failures == 0;
    std::printf("%s\n", ok ? "transcript_test: ALL PASS" : "transcript_test: FAILURES");
    return ok ? 0 : 1;
}
