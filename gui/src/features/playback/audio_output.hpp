#pragma once

#include <atomic>
#include <cstdint>
#include <memory>

#include "audio_sink.hpp"

namespace canvas::gui {

// Minimal audio sink that plays interleaved float PCM in [-1, 1]. Tries PipeWire
// first, then falls back to ALSA.
class AudioOutput : public AudioSink {
public:
    AudioOutput();
    ~AudioOutput() override;
    AudioOutput(const AudioOutput&) = delete;
    AudioOutput& operator=(const AudioOutput&) = delete;

    bool open(int sample_rate, int channels) override;
    void close() override;
    [[nodiscard]] bool is_open() const override { return open_; }

    // Writes `frames` interleaved frames (frames * channels floats total).
    // Best-effort: never blocks indefinitely; returns false if data was dropped
    // (e.g. output buffer overflowed). Samples are scaled by the current
    // monitoring volume (silenced while muted) before queuing.
    bool write_float(const float* data, int frames) override;
    void flush() override;
    // Scrub reposition: discard ALL pending + device-buffered audio and queue `data`
    // as the next thing the device plays. Unlike flush() it does NOT stop/join
    // the writer thread, so it's cheap enough for per-scrub-move use: it clears
    // the queue and issues snd_pcm_drop+prepare while the writer stays alive; the
    // writer discards any in-flight stale batch via a generation counter. This
    // makes the audible position JUMP to a new scrub position.
    bool reposition_enqueue(const float* data, int frames) override;
    // Interleaved frames currently queued (not yet handed to the device).
    [[nodiscard]] std::size_t pending_frames() const override;

    // Monitoring volume in [0.0, 1.0], applied to every queued sample. Setting this
    // un-mutes. 0.0 == silent.
    void set_volume(float volume);
    [[nodiscard]] float volume() const { return volume_.load(); }
    // Mute toggles the output on/off without changing the stored volume.
    // Un-muting restores the previous volume level.
    void set_muted(bool muted);
    [[nodiscard]] bool muted() const { return muted_.load(); }
    // Dim: temporarily dips monitoring volume to kDimGain x the current level (a
    // lighter dip than mute). Independent of the slider and of mute;
    // effective_volume() folds all three in.
    void set_dimmed(bool dimmed);
    [[nodiscard]] bool dimmed() const { return dimmed_.load(); }
    [[nodiscard]] float effective_volume() const;

    // While true (live playback), the ALSA writer keeps the device topped up with
    // silence between real audio chunks so it never underruns (XRUN corrupts
    // sync). Toggle off when paused/stopped so the device can idle instead of
    // feeding silence indefinitely.
    void set_hold_active(bool on) override;

    // ---- Always-on pipeline statistics (for diagnosing silent audio) ----
    uint64_t stat_enqueued_frames() const;
    uint64_t stat_dropped_frames() const;
    uint64_t stat_written_frames() const override;
    uint64_t stat_write_errors() const;
    // Silence holds fed by the writer (device kept alive between real chunks) and
    // ALSA XRUN recoveries since open. Exposed for the [audio:feed] / pipeline
    // stats line so starvation-vs-underrun is attributable.
    [[nodiscard]] uint64_t stat_silence_holds() const;
    [[nodiscard]] uint64_t stat_silence_hold_frames() const;
    [[nodiscard]] uint64_t stat_xruns() const;

    // Approximate media-time position (in frames at the opened sample_rate) that is
    // currently AUDIBLE at the speaker — accounts for DMA/device data ahead of the
    // writei cursor (snd_pcm_delay). ~0 if unmeasurable. The audio "master clock"
    // read side for A/V sync.
    uint64_t audible_position_frames() const override;
    // Emits a "audio: <tag> pipeline stats ..." line (always visible).
    void log_pipeline_stats(const char* tag) const override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    bool open_ = false;
    int write_log_ticks_ = 0;  // Debugging: throttle per-call audio write logs.
    std::atomic<float> volume_{0.8f};  // Default monitoring volume.
    std::atomic<bool> muted_{false};
    std::atomic<bool> dimmed_{false};
};

}
