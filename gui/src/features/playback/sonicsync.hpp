#pragma once

// SonicSync — A/V clock reconciliation ("audio rides with its frame").
//
// WHY IT EXISTS
//   Classic editors keep audio and video in lock-step because a single
//   read-ahead thread produces each position's audio AND picture together, and
//   the audio hardware callback is the master clock that paces the whole
//   pipeline: audio for position N can't reach the speaker until frame N is
//   decoded and popped. A live seek purges the queue and re-produces from the
//   new position, so audio and picture re-anchor together — audio never outruns
//   a frozen picture.
//
//   Here the worker paces the picture on its own timer while a separate audio
//   feed streams realtime. On a seek-while-playing, handle_seek rewinds and
//   pre-rolls audio at the target BEFORE the slow full-res frame is decoded, so
//   audio shoots ahead of the frozen picture — the persistent hundreds-of-ms
//   "audio ahead" error we observed, with no cheap feedback loop to pull it
//   back.
//
//   The policy:
//     * AUDIO RIDES WITH ITS FRAME — after a seek-while-playing the audio feed
//       is HELD (gated off) until the newly decoded target frame is presented.
//       Only then does audio for the new position begin, re-anchoring the two
//       together. No continuous loop, no lookahead rebuild, no per-frame cost.
//     * MASTER CLOCK — all video drop/hold decisions key off the *audible*
//       audio position, never a free-running wall clock, so the picture tracks
//       the speaker instead of racing it.
//     * OWNED DIAGNOSTICS — the audible-position derivation, run id and the
//       av_offset_ms math live here, so the sync log reports one truth.
//
// Threading: only the worker thread calls these. Values are plain members.
//
// FROZEN API: this public surface is the stable playback seam. Changes to
// existing signatures require the refactor plan's sign-off; new additive
// methods are fine.

#include <cstdint>

#include <canvas/core/project/project.hpp>

namespace canvas::gui {

class SonicSync {
public:
    // -------------------------------------------------------------------------
    // Seek-hold ("audio rides with its frame")
    // -------------------------------------------------------------------------

    // Begin a seek while playing. The caller is about to decode the target frame
    // (slow) with the picture frozen on the old frame; we cannot let new-position
    // audio stream during that stall. `target` is the committed seq frame.
    void begin_seek_hold(std::int64_t target);

    // Called once the newly decoded target frame has actually been presented.
    // Releases the audio hold so the new-position audio may begin feeding. After
    // this, audio and picture are re-anchored together (MLT purge semantics).
    void end_seek_hold();

    // Freeze a seek that is NOT while playing (paused scrub / transport). Audio
    // is irrelevant then; just record the position so a later play re-anchors.
    void on_seek_paused(std::int64_t target);

    // True while we are in the seek-hold window (seek-while-playing, target frame
    // not yet presented). While true the caller must NOT feed per-frame audio —
    // it would outrun the frozen picture.
    [[nodiscard]] bool seek_hold_active() const { return hold_active_ && !hold_released_; }

    // True any seek/scrub has happened at all within the current play run (used
    // to suppress stale audio from a pre-seek anchor).
    [[nodiscard]] bool pending_reanchor() const { return pending_reanchor_; }
    void confirm_reanchor() { pending_reanchor_ = false; }

    // -------------------------------------------------------------------------
    // Master clock: video reconciles to audible audio
    // -------------------------------------------------------------------------

    // Decide which video frame to actually present given the desired next frame,
    // the audible audio position as a seq frame (`aud_seq`, -1 unknown) and a
    // ceiling: video may sit up to `video_lead` frames past the audible position
    // in steady state (decode-ahead + device latency), and never more, or it
    // races ahead of what's audible. When the picture is behind audio it just
    // presents `want`.
    std::int64_t reconcile(std::int64_t want, std::int64_t aud_seq,
                           std::int64_t video_lead, std::int64_t total) const;

private:
    // Seek-hold state.
    bool hold_active_ = false;
    bool hold_released_ = false;
    std::int64_t hold_target_ = 0;
    bool pending_reanchor_ = false;
};

}  // namespace canvas::gui
