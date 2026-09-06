#pragma once

// Qt-free audio decode/feed pipeline for SequenceController (Phase 12 extraction).
//
// Owns everything audible: the per-media AudioDecoder set, the playhead<->sample
// conversions, the A/V sync run anchors (audio_rewind bookkeeping), the feed
// watermark, the audible-scrub grains, and the playing-scrub reposition feed.
// Talks to the device exclusively through the abstract AudioSink, so this unit
// stays display-free and can be tested headless with a mock sink.
//
// Callers: the controller worker thread (play_step / rewind / preroll / close /
// scrub grain) and the UI thread (feed_scrub_audio per mouse-move). Decoder
// access is mutex-guarded; the sink itself is internally thread-safe.
//
// FROZEN API (splitplan Phase 22): this public surface is the stable playback
// seam. Changes to existing signatures require the refactor plan's sign-off;
// new additive methods are fine.

// Forward declaration keeps this header lean for the sink dependency (held by
// reference, complete type only needed in the .cpp). The per-media decoders
// stay a direct include: unordered_map<unique_ptr<Incomplete>> cannot be
// default-constructed in the header, and audio_decoder.hpp is Qt-free anyway.
#include "canvas/core/media/audio_decoder.hpp"
#include "canvas/core/project/project.hpp"

#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <unordered_map>

namespace canvas::gui {

class AudioSink;

class AudioPipeline {
public:
    explicit AudioPipeline(AudioSink& sink) : sink_(sink) {}
    ~AudioPipeline();
    AudioPipeline(const AudioPipeline&) = delete;
    AudioPipeline& operator=(const AudioPipeline&) = delete;

    // Project the decode/feed queries are made against (raw borrow; the owning
    // controller keeps the shared_ptr alive across any pipeline call).
    void set_project(const canvas::core::Project* project);
    // Live swap of the project the mix reads from (no decoder reset, no device
    // flush): the next mixed buffer picks up the new per-clip volume/pan/fade
    // values, so audio edits apply in realtime during playback.
    void update_project(const canvas::core::Project* project);
    // Open an audio decoder for a media entry (no-op unless it has audio).
    void add_media(const canvas::core::MediaEntry& entry);
    // Tear down for a project swap: flush+drain any active output, close all
    // decoders, drop anchors/project.
    void reset();

    [[nodiscard]] bool is_active() const { return active_; }
    [[nodiscard]] bool is_open() const;
    [[nodiscard]] AudioSink& sink() { return sink_; }

    // Open (once) the output device at the pipeline rate/channel config.
    void open_output();
    void close_output();

    // Re-arm the output at `seq_frame` for a fresh play run: bump the run id,
    // anchor the audible-position bookkeeping, reset the per-media feed
    // watermark, reset all decoders, and flush the device so playback resumes
    // from this position (never stale audio from the previous run).
    void rewind(int64_t seq_frame, bool playing);

    // Write `lead_ms` of audio at `seq_frame` ahead of the first present so the
    // AUDIBLE cursor (written minus device latency) starts aligned with the
    // picture instead of lagging by the fixed device buffer.
    void preroll(int64_t seq_frame, int lead_ms, bool playing);

    // Steady-playback feed: decode the per-frame span at `seq_frame` and write
    // it to the device, honoring SonicSync's seek-hold (passed in as
    // `seek_hold_active`) and the feed watermark so pre-rolled audio is never
    // re-written. Emits [avsync] + [audio] health.
    void play_step(int64_t seq_frame, double step_seconds, bool seek_hold_active);

    // Audible scrub (paused): write a short PCM grain at `seq_frame`.
    void play_scrub_grain(int64_t seq_frame);

    // Playing-scrub reposition feed (UI thread, per mouse-move): reposition the
    // device onto a ~120ms chunk decoded at `target`. Throttled to ~45ms and
    // coalesced to the last target; no writer stop/join.
    void feed_scrub_audio(int64_t target);

    // Per-drag scrub telemetry (reset by begin_scrub, reported by the controller
    // in its [scrub] END line).
    void begin_scrub();
    [[nodiscard]] int repositions_since_begin() const { return scrub_repositions_; }

    // === A/V sync reads (anchored to the last rewind) ===
    // The audible timeline seq frame, or -1 when nothing is trusted to be
    // audible at `playhead_seq` (no live enabled audio clip under it, or the
    // anchor math yields nothing usable).
    int64_t audible_seq_frame(int64_t playhead_seq) const;
    // Feed watermark: whether the current play run has pre-rolled audio for a
    // media (i.e. a watermark lives in some clip's audio domain).
    [[nodiscard]] bool feed_watermark_active() const { return !feed_watermarks_.empty(); }
    // After a drop-to-realtime playhead jump, re-anchor the feed watermark to
    // `new_frame` so the next play_step produces audio at realtime only.
    void advance_feed_for_drop(int64_t new_frame);

    // Media of the audio clip covering `seq_frame`, or -1. Used by the master
    // clock gate to decide whether anything is actually audible at a position.
    canvas::core::MediaId audio_media_at(int64_t seq_frame) const;

private:
    struct AudioSource {
        const canvas::core::Clip* clip = nullptr;
        double fps = 0.0;
    };

    // The master/reference clip for A/V sync: the first audible source
    // (lowest-lane audio-track clip wins; the video track's embedded audio is
    // the master only when no audio track covers the playhead).
    const canvas::core::Clip* audio_clip_at(int64_t seq_frame) const;
    // Every clip audible at `seq_frame` after the mute/solo filter: audio tracks
    // in playback order (bottom lane first), then the video tracks' embedded
    // audio when NO audio-track clip covers the playhead and nothing is soloed.
    std::vector<AudioSource> audible_sources_at(int64_t seq_frame) const;
    // Writes `want_frames` frames of the summed mix of every audible source at
    // `seq_frame` to the device, honoring the per-media feed watermark so
    // pre-rolled audio is never re-written. Per-source clip Volume/Pan (equal-
    // power) and the audio-transition fade envelopes are applied before summing.
    // Returns the frames actually written (0 = nothing audible / no decode).
    int64_t write_mixed(int64_t seq_frame, int64_t want_frames);

    const canvas::core::Project* project_ = nullptr;
    const canvas::core::Clip* clip_at_any_track(int64_t seq_frame) const;
    int64_t playhead_to_audio_sample(int64_t seq_frame) const;
    int64_t audio_sample_to_seq_frame(int64_t media_sample) const;
    // Debug: log video position vs audible audio position for A/V sync.
    void log_av_sync(int64_t seq_frame, double video_fps, double step_seconds);

    AudioSink& sink_;

    // Independent audio decoders (one per media), so audio decoding never
    // disturbs concurrent video decoding of the same file.
    std::unordered_map<canvas::core::MediaId, std::unique_ptr<canvas::core::AudioDecoder>>
        decoders_;
    mutable std::mutex mutex_;

    int rate_ = 48000;
    int channels_ = 2;
    bool active_ = false;

    // A/V sync diagnostic anchors (protected by mutex_). Each time audio is
    // re-armed we record the media sample the run starts at and the device's
    // written-frame counter at that moment, so the *audible* position within the
    // run can be derived and compared to the video position being presented.
    int64_t anchor_media_sample_ = 0;
    uint64_t written_at_anchor_ = 0;
    uint64_t run_id_ = 0;
    uint64_t speed_run_ = 0;
    double speed_video_ms_ = 0.0;
    double speed_audible_ms_ = 0.0;

    // Next media-sample (per media, in that media's sample domain) still needing
    // to be written to the output device within the current play run. preroll()
    // and play_step() both advance these so leading audio pre-rolled before the
    // first present is never written twice — per media so each mixed source
    // keeps its own lead-in.
    std::unordered_map<canvas::core::MediaId, int64_t> feed_watermarks_;

    // UI-thread scrub-audio feed state (feed_scrub_audio): last target fed and
    // when, for throttling to device pace and de-duplication, plus the count of
    // successful audible repositions within the current drag.
    int64_t last_scrub_audio_target_ = -1;
    std::chrono::steady_clock::time_point last_scrub_audio_at_{};
    int scrub_repositions_ = 0;
};

}  // namespace canvas::gui