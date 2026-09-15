// Unit tests for the Qt-free caption-shaping law (canvas::core::captions):
// the wrap/chunk law, the char-weighted time split, the gap enforcement and
// the preset catalogue. Pure math — no model, no devices → always runs.

#include "canvas/core/timeline/captions.hpp"

#include <cstdio>
#include <cstdlib>
#include <string>

#include "canvas/core/media/transcript.hpp"

namespace {

int g_failures = 0;

void report(const bool ok, const char* name) {
    std::printf("%s  %s\n", ok ? "PASS" : "FAIL", name);
    if (!ok) ++g_failures;
}

}  // namespace

int main() {
    using canvas::core::captions::Caption;
    using canvas::core::captions::Options;
    using canvas::core::captions::preset_options;
    using canvas::core::captions::presets;
    using canvas::core::captions::sanitize;
    using canvas::core::captions::shape_captions;
    using canvas::core::captions::wrap_line;
    using canvas::core::transcript::Segment;

    // --- preset catalogue ----------------------------------------------------
    report(!presets().empty(), "presets: catalogue is non-empty");
    report(preset_options("standard").max_chars_per_line == 42,
           "presets: standard -> 42 chars/line, 1 line");
    report(preset_options("standard").max_lines == 1, "presets: standard -> 1 line");
    report(preset_options("standard").gap_frames == 0, "presets: standard -> 0 gap");
    report(preset_options("classic").max_lines == 2 && preset_options("classic").gap_frames == 2,
           "presets: classic -> 2 lines + 2-frame gap");
    report(preset_options("social").max_chars_per_line == 32,
           "presets: social -> 32 chars/line");
    report(preset_options("burned").max_chars_per_line == 58,
           "presets: burned -> 58 chars/line");
    report(preset_options("nonexistent").max_chars_per_line == 42,
           "presets: unknown name falls back to standard");

    // --- clamp law -----------------------------------------------------------
    Options bad{0, 7, -3};
    const Options fixed = sanitize(bad);
    report(fixed.max_chars_per_line == 4, "clamp: chars floor at 4");
    report(fixed.max_lines == 3, "clamp: lines capped at 3");
    report(fixed.gap_frames == 0, "clamp: gap floored at 0");

    // --- wrap law ------------------------------------------------------------
    report(wrap_line("hello world", 42) == "hello world", "wrap: short line unchanged");
    report(wrap_line("hello world", 5) == "hello\nworld", "wrap: breaks at column 5");
    report(wrap_line("one two three four five", 10) == "one two\nthree four\nfive",
           "wrap: cascades across three lines");
    report(wrap_line("supercalifragilistic", 4) == "supercalifragilistic",
           "wrap: unbreakable token keeps full width on its own line");

    // --- shape: single short segment ----------------------------------------
    const std::vector<Segment> one{{{0, 2000, "Hello world"}}};
    const std::vector<Caption> out1 = shape_captions(one, {42, 1, 0}, 30.0);
    report(out1.size() == 1, "shape: single fitting segment -> one caption");
    report(out1.size() == 1 && out1[0].text == "Hello world",
           "shape: text passes through untouched");
    report(out1.size() == 1 && out1[0].start_ms == 0 && out1[0].end_ms == 2000,
           "shape: span kept as-authored");

    // --- shape: one long segment splits with char-weighted time --------------
    // 90 chars over 9000 ms under 42/1 -> three captions (~30 chars each) each
    // getting ~1/3 of the span.
    const std::string long_word = [] {
        std::string s = "alpha ";
        while (static_cast<int>(s.size()) < 90) s += "word ";
        return s.substr(0, s.find_last_of(' ', 89));
    }();
    const std::vector<Segment> lon{{{0, 9000, long_word}}};
    const std::vector<Caption> out2 = shape_captions(lon, {42, 1, 0}, 30.0);
    report(out2.size() >= 2, "shape: long segment splits into >= 2 captions");
    if (out2.size() >= 2) {
        for (std::size_t i = 0; i < out2.size(); ++i) {
            report(static_cast<int>(out2[i].text.size()) <= 42,
                   "shape: every caption holds <= 42 chars");
        }
        report(out2.front().start_ms == 0, "shape: first caption opens at segment start");
        report(out2.back().end_ms == 9000, "shape: last caption closes at segment end");
        const int64_t mid = (out2[0].end_ms + out2[1].start_ms) / 2;
        report(std::llabs(out2[1].start_ms - out2[0].end_ms) <= 1,
               "shape: captions are back-to-back");
        report(out2[1].start_ms > 0 && out2[1].start_ms < 9000, "shape: split lands mid-span");
        (void)mid;
    }

    // --- shape: multi-line policy merges --------------------------------
    const std::string eighty = [] {
        std::string s;
        while (static_cast<int>(s.size()) < 80) s += "word ";
        return s.substr(0, s.find_last_of(' ', 79));
    }();
    const std::vector<Segment> two{{0, 4000, eighty}};
    const std::vector<Caption> out3 = shape_captions(two, {42, 2, 0}, 30.0);
    report(out3.size() == 1, "shape: 80 chars fit one 2-line caption");
    report(out3.size() == 1 && out3[0].text.find('\n') != std::string::npos,
           "shape: 2-line caption carries a newline");

    // --- shape: gap enforcement ---------------------------------------------
    // Two 1-second cues with a 100 ms natural gap; requiring 6 frames @ 30 fps
    // (200 ms) pushes the second cue's start forward to 1200 ms.
    const std::vector<Segment> gaps{{{0, 1000, "First"}, {1100, 2100, "Second"}}};
    const std::vector<Caption> out4 = shape_captions(gaps, {42, 1, 6}, 30.0);
    report(out4.size() == 2, "gap: both cues survive 6-frame enforcement");
    report(out4.size() == 2 && out4[1].start_ms == 1200,
           "gap: second cue start pulled from 1100 to 1200 ms");
    report(out4.size() == 2 && out4[0].start_ms == 0 && out4[0].end_ms == 1000,
           "gap: first cue untouched");

    // --- shape: empty / zero-length segments are skipped ---------------------
    const std::vector<Segment> junk{{{0, 0, ""},
                                     {100, 900, "   \t "},
                                     {1000, 3000, "Keep me"}}};
    const std::vector<Caption> out5 = shape_captions(junk, {42, 1, 0}, 30.0);
    report(out5.size() == 1 && out5[0].text == "Keep me",
           "shape: empty + whitespace + zero-length segments are skipped");

    std::printf("\n%d failure(s)\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}