#pragma once

// Pitch-preserving per-clip time-stretch (WSOLA), Qt-free, in canvas_core so
// playback (AudioPipeline) and export (RenderSession) apply the SAME retime
// law after Speed Change (clip_rate). Replaces the old linear-interp
// varispeed resampler: retiming a clip must change its TEMPO, not its pitch,
// so a 2x clip sounds like the same recording talking twice as fast — not
// helium. Pitch is preserved by construction: the
// output is a stream of windowed grain copies of the source waveform, packed
// at a synthesis hop that differs from the analysis hop, so the output
// waveform tracks the INPUT waveform sample-true and only the number of
// samples per second changes.
//
// Pipeline position (both call sites): decode contiguous media -> stretch ->
// RNNoise voice isolation -> fades/volume/pan mix. The stretch consumes
// `ratio` source frames per output frame (ratio == cliprate::effective_rate),
// so both audio paths stay locked to the same timeline law the video path
// uses: A/V sync is intact by construction.
//
// Engine contract (streaming):
//  - process() feeds interleaved float PCM in the CONTIGUOUS continuation of
//    every previous call for the same engine (the callers decode forward
//    monotonically). A discontinuity (seek / clip re-open / rate change) must
//    go through drop()/reset() first — the engine does not splice stale
//    analysis state across a jump.
//  - process() writes at most `out_frames` output frames and returns the
//    count actually written. A short return is NORMAL: it happens while the
//    engine primes its first window (~40 ms of history exists before the
//    first grain can be placed) and again at chunk seams whenever the caller
//    feeds exactly `out_frames * ratio` media with no lookahead — the last
//    grain's window reaches into the next chunk's data, so it is deferred.
//    Both callers are built to tolerate this (RNNoise does the same today);
//    callers that need an EXACT count (export) should feed an extra
//    `TimeStretch::lookahead_frames(sample_rate)` media frames so every grain
//    completes within the chunk.
//  - reset() drops every streaming state (analysis/synthesis cursors, history
//    buffer, OLA tails); the next process() starts a brand-new stream.

#include <cstdint>
#include <map>
#include <vector>

namespace canvas::core {

// Streaming WSOLA time-stretch, one instance per active clip (normally held
// via TimeStretchBank). Not thread-safe; single-threaded per bank entry.
class TimeStretch {
public:
    TimeStretch() = default;
    ~TimeStretch() = default;
    TimeStretch(const TimeStretch&) = delete;
    TimeStretch& operator=(const TimeStretch&) = delete;
    TimeStretch(TimeStretch&&) noexcept = default;
    TimeStretch& operator=(TimeStretch&&) noexcept = default;

    // Extra media frames (at `sample_rate`) to feed alongside
    // `out_frames * ratio` so the engine can always complete every grain in
    // the same call and return the exact requested count. ~40 ms at 48 kHz.
    [[nodiscard]] static int lookahead_frames(int sample_rate) noexcept;

    // Stretch `in` (`in_frames` * `channels`, interleaved, contiguous
    // continuation of previous calls) into `out` (resized to
    // `out_frames * channels`). `sample_rate` sizes the analysis window and
    // must be constant per engine (it can still be any value; the stretch has
    // no 48 kHz constraint). `ratio` is media frames consumed per output
    // frame. Returns the number of output frames actually written
    // (<= out_frames); `out` is only meaningful up to that count.
    [[nodiscard]] int process(const float* in, int in_frames, int channels,
                              double ratio, int out_frames, int sample_rate,
                              std::vector<float>& out);

    // Drop all streaming state; the next process() starts a fresh stream.
    void reset();

private:
    void setup(int sample_rate, int channels);
    void compact_history();
    int search_best_offset(int64_t candidate) const;

    std::vector<float> window_;  // W-sample Hann fade
    std::vector<float> in_hist_; // retained media, interleaved, from in_base_
    std::vector<float> cur_win_; // current grain's windowed samples (W*ch)
    std::vector<float> prev_tail_;  // previous grain's windowed [S..W) (S*ch)
    std::vector<float> prev_sig_;   // previous grain's raw input tail [S..W) (S*ch)

    int sample_rate_ = 0;
    int channels_ = 0;
    int W_ = 0;  // analysis/synthesis window, samples
    int S_ = 0;  // synthesis hop = W/2 (50% overlap, Hann sums flat)
    int D_ = 0;  // analysis search radius, samples

    int64_t in_base_ = 0;  // absolute media frame of in_hist_[0]
    int64_t cur_s_ = 0;    // absolute output frame of current grain window start
    int64_t s_ = 0;        // absolute output lattice: next grain goes here
    int64_t out_base_ = 0; // absolute output frame the next call's out[0] maps to
    double a_ = 0.0;       // nominal analysis cursor for the next grain
    bool first_ = true;    // no grain placed on this stream yet
};

// Small per-clip registry so the realtime and export paths can run one WSOLA
// stream continuously across calls (keyed by ClipId), mirroring
// VoiceIsolationBank. tick() every clip; drop() when the timeline position
// jumps out of sequence (seek/re-anchor) or the ratio changes, so analysis
// state never carries across a discontinuity.
class TimeStretchBank {
public:
    // Stretch `in` for `clip_id` into `out`. `ratio` must stay constant per
    // clip id (a change resets the engine defensively). Returns frames
    // written (<= out_frames); see TimeStretch::process for the short-return
    // contract.
    [[nodiscard]] int tick(std::uint64_t clip_id, double ratio, int sample_rate,
                           int channels, const float* in, int in_frames, int out_frames,
                           std::vector<float>& out);

    void drop();               // reset all per-clip stretch state
    void drop(std::uint64_t clip_id);  // reset one clip's state
    void clear();              // release every per-clip state object

private:
    struct Entry {
        TimeStretch engine;
        double ratio = 0.0;
    };
    std::map<std::uint64_t, Entry> entries_;
};

}  // namespace canvas::core