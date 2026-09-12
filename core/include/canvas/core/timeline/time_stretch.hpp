#pragma once

// Per-clip retime + pitch + pan engine (WSOLA + windowed-sinc resampling),
// Qt-free, in canvas_core so playback (AudioPipeline) and export
// (RenderSession) apply the SAME per-clip audio law. Speed Change keeps PITCH
// preserved by construction — the output is a stream of windowed grain copies
// of the source waveform packed at a synthesis hop that differs from the
// analysis hop, so only the number of samples per second changes; Pitch shift
// (clip_rate::pitch_factor) runs a windowed-sinc resampling FRONT-END ahead of
// the WSOLA stage; the engine's synthesis ratio is then `speed_ratio /
// pitch_factor`, so the two cancel and the final output length obeys ONLY the
// speed law while the pitch lands on the shifted factor. Pan is applied to the
// engine output through the shared audio_mix::pan_gains balance law, so a
// mono source with a non-center pan is upmixed to the front pair here and the
// downstream mix always applies center (gl = gr = 1).
//
// Pipeline position (both call sites): decode contiguous media -> retime
// (pitch -> WSOLA -> pan) -> RNNoise voice isolation -> fades/volume mix. The
// engine consumes `speed_ratio` source frames per output frame (ratio ==
// cliprate::effective_rate), so both audio paths stay locked to the same
// timeline law the video path uses: A/V sync is intact by construction.
//
// Stage bypasses:
//  - pitch == 1.0: the resampler is skipped entirely (the input history IS the
//    WSOLA ring), keeping the speed-only path bit-identical to the pre-pitch
//    engine;
//  - |speed_ratio - pitch_factor| ~ 0 (e.g. speed 2x with +12 st): the WSOLA
//    stage is bypassed (its unity-ratio overlap-add would corrupt an SRC'd
//    stream) and the resampled output streams straight through;
//  - pan == 0.0 && pitch == 1.0 && speed 1.0: a plain passthrough copy, so
//    identity clips are never touched by any stage.
//
// Engine contract (streaming):
//  - process() feeds interleaved float PCM in the CONTIGUOUS continuation of
//    every previous call for the same engine (the callers decode forward
//    monotonically). A discontinuity (seek / clip re-open / rate or pitch
//    change) must go through drop()/reset() first — the engine does not
//    splice stale analysis state across a jump.
//  - process() writes at most `out_frames` output frames and returns the
//    count actually written. A short return is NORMAL while the engine primes
//    its first window (~40 ms of history exists before the first grain can be
//    placed) and again at chunk seams whenever the caller feeds exactly
//    `out_frames * ratio` media with no lookahead. Both callers are built to
//    tolerate this; callers that need an EXACT count (export) should feed an
//    extra `TimeStretch::retime_lookahead(sample_rate, ratio, pitch)` media
//    frames so every grain completes within the chunk.
//  - out_channels() reports the channel count actually written this call: it
//    equals the input channels except for a mono source with pan != 0, which
//    is upmixed to 2.
//  - reset() drops every streaming state (analysis/synthesis cursors, history
//    buffers, OLA tails); the next process() starts a brand-new stream.

#include <cstdint>
#include <map>
#include <vector>

namespace canvas::core {

// Streaming retime/pitch/pan engine, one instance per active clip (normally
// held via TimeStretchBank). Not thread-safe; single-threaded per bank entry.
class TimeStretch {
public:
    TimeStretch() = default;
    ~TimeStretch() = default;
    TimeStretch(const TimeStretch&) = delete;
    TimeStretch& operator=(const TimeStretch&) = delete;
    TimeStretch(TimeStretch&&) noexcept = default;
    TimeStretch& operator=(TimeStretch&&) noexcept = default;

    // Extra media frames (at `sample_rate`) the WSOLA stage needs alongside
    // `out_frames * ratio` to always complete every grain in the same call and
    // return the exact requested count. ~40 ms at 48 kHz.
    [[nodiscard]] static int lookahead_frames(int sample_rate) noexcept;

    // Resampling front-end edge margin (media frames the windowed-sinc can
    // reach past its current cursor). Rate-independent.
    [[nodiscard]] static int src_margin_frames() noexcept;

    // Total extra media frames (at `sample_rate`) to feed on top of
    // `out_frames * speed_ratio` so the engine returns the exact requested
    // count in the same call for any (ratio, pitch) pair. 0 when the clip is
    // fully identity; exactly `lookahead_frames` for the speed-only case
    // (pitch == 1.0); scaled by `pitch_factor` when the resampler runs.
    [[nodiscard]] static int retime_lookahead(int sample_rate, double speed_ratio,
                                              double pitch_factor) noexcept;

    // Feed `in` (`in_frames` * `channels`, interleaved, contiguous
    // continuation of previous calls) into `out` (resized to
    // `out_frames` * out_channels()). `sample_rate` sizes the analysis window
    // and must be constant per engine (it can still be any value; the stretch
    // has no 48 kHz constraint). `speed_ratio` is media frames consumed per
    // output frame (cliprate::effective_rate); `pitch_factor` the resample
    // factor (1.0 = none); `pan` the per-clip balance [-1, 1]. Returns the
    // number of output frames actually written (<= out_frames); `out` is only
    // meaningful up to that count.
    [[nodiscard]] int process(const float* in, int in_frames, int channels,
                              double speed_ratio, double pitch_factor, float pan,
                              int out_frames, int sample_rate,
                              std::vector<float>& out);

    // Channel count in `out` from the most recent process() (input channels,
    // or 2 for a mono source that was panned).
    [[nodiscard]] int out_channels() const noexcept { return out_channels_; }

    // Drop all streaming state; the next process() starts a fresh stream.
    void reset();

private:
    static constexpr int kSrcTapsHalf = 16;  // windowed-sinc taps each side
    static double sinc(double x) noexcept;

    void setup(int sample_rate, int channels);
    void compact_history();                 // WSOLA ring: drop behind a_ - D_
    void compact_media();                   // media ring: drop behind SRC cursor
    void emit_src(int64_t media_end);       // resample media_hist_ -> in_hist_
    int search_best_offset(int64_t candidate) const;  // WSOLA offset search
    int run_synth(int out_frames);          // WSOLA lattice, writes stage_
    int run_passthrough(int out_frames);    // stream in_hist_ -> stage_
    void finalize(std::vector<float>& out, int written, float pan);

    std::vector<float> window_;  // W-sample Hann fade
    std::vector<float> in_hist_; // WSOLA/SRC-output ring, interleaved, from in_base_
    std::vector<float> media_hist_;  // RAW media ring, interleaved, from media_base_
    std::vector<float> stage_;       // engine-lane output before pan/expand
    std::vector<float> cur_win_; // current grain's windowed samples (W*ch)
    std::vector<float> prev_tail_;  // previous grain's windowed [S..W) (S*ch)
    std::vector<float> prev_sig_;   // previous grain's raw input tail [S..W) (S*ch)

    int sample_rate_ = 0;
    int channels_ = 0;
    int out_channels_ = 0;
    int W_ = 0;  // analysis/synthesis window, samples
    int S_ = 0;  // synthesis hop = W/2 (50% overlap, Hann sums flat)
    int D_ = 0;  // analysis search radius, samples

    int64_t in_base_ = 0;      // absolute frame of in_hist_[0]
    int64_t media_base_ = 0;   // absolute MEDIA frame of media_hist_[0]
    double src_pos_ = 0.0;     // absolute media position of the next SRC sample
    double pitch_ = 1.0;       // current pitch factor (SRC media step per sample)
    double wsola_ratio_ = 1.0; // SRC-output frames consumed per output frame (spd/pitch)
    int64_t cur_s_ = 0;    // absolute output frame of current grain window start
    int64_t s_ = 0;        // absolute output lattice: next grain goes here
    int64_t out_base_ = 0; // absolute output frame the next call's out[0] maps to
    double a_ = 0.0;       // nominal analysis cursor for the next grain
    bool first_ = true;    // no grain placed on this stream yet
};

// Small per-clip registry so the realtime and export paths can run one retime
// stream continuously across calls (keyed by ClipId), mirroring
// VoiceIsolationBank. tick() every clip; drop() when the timeline position
// jumps out of sequence (seek/re-anchor) or the ratio OR pitch changes, so
// analysis state never carries across a discontinuity.
class TimeStretchBank {
public:
    // Retime `in` for `clip_id` into `out`. `speed_ratio` and `pitch_factor`
    // must stay constant per clip id (a change resets the engine
    // defensively). Returns frames written (<= out_frames); see
    // TimeStretch::process for the short-return contract. When
    // `out_channels` is non-null it receives the channel count actually
    // written (2 for a panned mono source).
    [[nodiscard]] int tick(std::uint64_t clip_id, double speed_ratio,
                           double pitch_factor, float pan, int sample_rate,
                           int channels, const float* in, int in_frames,
                           int out_frames, std::vector<float>& out,
                           int* out_channels = nullptr);

    void drop();               // reset all per-clip stretch state
    void drop(std::uint64_t clip_id);  // reset one clip's state
    void clear();              // release every per-clip state object

private:
    struct Entry {
        TimeStretch engine;
        double ratio = 0.0;
        double pitch = 1.0;
    };
    std::map<std::uint64_t, Entry> entries_;
};

}  // namespace canvas::core