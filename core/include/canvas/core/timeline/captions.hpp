#pragma once

// Qt-free caption-shaping law for timed-text overlays (the AI-subtitle
// pipeline). Given whisper transcript segments (timed text in wall-clock
// milliseconds) plus a caption-style policy, this module produces the flat,
// non-overlapping caption stream that becomes on-timeline title-clip bars:
//   - every caption's text is word-wrapped to <= max_chars_per_line columns,
//     laid out over up to `max_lines` lines (so a caption holds at most
//     max_chars_per_line * max_lines characters);
//   - a segment whose text overflows one caption is split, and the segment's
//     wall-clock span is divided among its captions proportionally to the
//     character weight of each piece (the standard sub-robot pacing law);
//   - consecutive captions keep a minimum `gap_frames` empty interval at the
//     sequence's frame rate so the eye can re-acquire the next caption.
//
// Lives beside the SRT seam (canvas::core::transcript) that feeds it, staying
// deliberately free of both Qt and the whisper dependency so the shaping law
// is unit-testable in isolation and the engine can be swapped.

#include "canvas/core/media/transcript.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace canvas::core::captions {

// One shaped on-screen caption in wall-clock milliseconds. `text` holds a
// single newline between lines when the policy allows max_lines > 1.
struct Caption {
    int64_t start_ms = 0;
    int64_t end_ms = 0;
    std::string text;
};

// Caption-style policy. The GUI's AI-tools dialog edits the raw values
// directly ("Custom"); the named presets below are the one-click menu rows.
struct Options {
    int max_chars_per_line = 42;  // YouTube-style caption column width
    int max_lines = 1;            // vertical span: 1 (single) .. 3 at most
    int64_t gap_frames = 0;       // enforced empty interval between captions
};

// Named caption presets (Standard / Classic Subtitle / Social Caption /
// Burned Caption). `name` is a stable identifier, not the localized label.
struct Preset {
    const char* name = nullptr;
    Options options;
};

// The preset catalogue; indexed/queried by name, not by position.
[[nodiscard]] const std::vector<Preset>& presets() noexcept;

// Preset lookup by stable name; unknown names return the Standard preset.
[[nodiscard]] const Options& preset_options(std::string_view name) noexcept;

// Clamps an options struct into its lawful envelope: max_chars_per_line into
// [4, 160], max_lines into [1, 3], gap_frames >= 0.
[[nodiscard]] Options sanitize(const Options& opts) noexcept;

// The shape law: turns timed transcript segments into the flat caption stream
// described in the file-top comment. `sequence_fps` drives the gap math; a
// non-positive value falls back to 30.0. Segments with empty cleaned text are
// skipped; zero-length closures after gap pushing are kept as-authored (the
// placement layer decides whether a 0-frame bar is worth dropping).
[[nodiscard]] std::vector<Caption> shape_captions(
    const std::vector<transcript::Segment>& segments, const Options& opts,
    double sequence_fps);

// Word-wraps `text` to lines of at most `max_chars` columns, joined with
// '\n'. A single word longer than the column falls back to its own line
// (unbreakable tokens keep all their characters). The line law the shape
// function uses internally, exported for the Inspector/toolbox previews.
[[nodiscard]] std::string wrap_line(std::string_view text, int max_chars);

}  // namespace canvas::core::captions