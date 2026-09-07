#pragma once

// Playback pacing / A/V sync tuning constants, separated from SequenceController
// so they can be unit-tested and edited without touching controller logic.
//
// Qt-free on purpose: sync policy math must run in headless tests (SonicSync,
// AudioPipeline) which cannot pull in Qt.

#include <cstddef>
#include <cstdint>

namespace canvas::gui {

// Decode-ahead lookahead depth: how many frames are decoded ahead of the playhead
// so presentation pops an already-ready frame instead of decoding inline. Also the
// master-clock ceiling — video may sit at most this many frames past the audible
// position before the drop-to-realtime cap holds it (see SonicSync::reconcile).
inline constexpr std::size_t kLookahead = 24;

// Full-res frames warmed ahead of a settled scrub target while playing, so
// releasing the drag resumes instantly from the ready buffer.
inline constexpr std::size_t kScrubPrecache = 4;

// Longest edge (px) of the reduced-resolution scrub preview. Scrubbing decodes a
// whole GOP per keyframe->target seek; ~640 keeps RGBA convert + buffer traffic
// ~16x cheaper than full-res 2560x1440 while remaining sharp enough to scrub by.
inline constexpr int kPreviewMaxDim = 640;

// Max forward frame delta for which a full-res prepared-playback decode may walk
// the decoder sequentially. Steady-state warming advances frame-by-frame (delta
// 0-1) and scrub resumes park the decoder at the target, both well under this.
// Larger forward jumps (a far scrub-release commit while the decoder is parked
// far behind) previously walked every frame between — seconds of worker stall
// that froze all drag previews behind it. Beyond the budget they anchor on the
// owning I-frame instead, bounding the walk to a single GOP.
inline constexpr std::int64_t kCommitSeqMaxDelta = 96;

// Milliseconds of leading audio written ahead of the picture before the first
// present, to compensate the fixed ALSA device-buffer latency so the audible
// audio lines up with the frame shown (see AudioPipeline::preroll). Device tunable.
inline constexpr int kAudioLeadMs = 120;

}  // namespace canvas::gui
