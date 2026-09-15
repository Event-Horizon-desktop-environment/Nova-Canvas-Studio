#pragma once

// Qt-free subtitle-serialization seam for the local voice-transcription
// pipeline (Phase A heads-up, splitplan-style headless module). Defined here:
// the SRT subtitle writer plus the cue-text normalization law that feeds it.
//
// The whisper.cpp engine that PRODUCES the Segment list lives behind this seam
// (a headless transcribe job on a 16 kHz mono resample, quantized model); this
// module stays deliberately free of both Qt and the whisper dependency so the
// format law is unit-testable in isolation and the engine can be swapped.

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace canvas::core::transcript {

// One captioned utterance in wall-clock milliseconds (0-based, anchors the
// timeline/subtitle mapping that the Deliver panel will expose).
struct Segment {
    int64_t start_ms = 0;
    int64_t end_ms = 0;
    std::string text;
};

// Wall-clock timestamp law, SRT shares the MS-TIME format HH:MM:SS,mmm.
// Negative values clamp to 00:00:00,000; values at/above 100 hours clamp to
// 99:59:59,999 (the format cannot express a hundred-th hour).
[[nodiscard]] std::string srt_timestamp(int64_t ms);

// Text law shared by the writer and the (future) subtitle preview UI:
// trims leading/trailing whitespace and collapses interior runs of whitespace
// (spaces, tabs, newlines) to a single space. Whitespace-only input yields an
// empty string so a silent cue serializes to nothing.
[[nodiscard]] std::string clean_segment_text(std::string_view raw);

// Serializes cues to SRT (UTF-8): 1-based cue numbers, `HH:MM:SS,mmm -->
// HH:MM:SS,mmm`, `\r\n` line endings (the SRT/Teletext convention), blank line
// between cues and a trailing blank line. Cues with empty cleaned text are
// skipped; zero-length/overlap cues are kept as-authored (the engine owns
// those semantics). Returns the full SRT text.
[[nodiscard]] std::string srt_join(const std::vector<Segment>& segments);

// Writes `srt_join(segments)` to `path` (UTF-8, binary mode). False on I/O
// failure; cue text is not escaped (arbitrary UTF-8 passes through).
bool write_srt(std::string_view path, const std::vector<Segment>& segments);

}  // namespace canvas::core::transcript