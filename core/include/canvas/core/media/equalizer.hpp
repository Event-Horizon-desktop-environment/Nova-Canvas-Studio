#pragma once

// Per-clip parametric equalizer: a small headless seam in canvas_core so
// playback (AudioPipeline) and export (RenderSession) apply the SAME 6-band EQ
// per clip before its gains/mix. Qt-free by construction.
//
// The engine is the RBJ Audio EQ Cookbook biquad (Bell/peaking EQ, LowShelf,
// HighShelf, LowPass, HighPass, Notch), cascaded band 0..5 per channel, run in
// Direct Form 2 transposed form (the numerically stable streaming form).
// Coefficient sets are computed in double precision from the shared band law
// (`Clip::EqBand`), stored normalized (a0 == 1) in float for the inner loop.
//
// Compared to the other per-clip audio seams:
//  - Unlike RNNoise, the EQ is rate-INDEPENDENT — biquads are computed per
//    sample rate, so the stage runs at any playback/export rate (no 48 kHz
//    bypass rule).
//  - Unlike RNNoise/WSOLA, the EQ has NO lookahead or priming: process()
//    writes exactly `num_frames` frames every call, in place, so the mix
//    path's frame accounting never changes.
//  - Bell/Shelf bands with |gain| <= 1e-3 dB are skipped (identity), so a
//    flat EQ is a bit-exact pass-through rather than a chain of unity filters;
//    LowPass/HighPass/Notch always filter. Band frequencies above 0.45*rate
//    are clamped into the valid range so no coefficient degenerates near
//    Nyquist (an export at a low rate with a 20 kHz band stays stable).

#include "canvas/core/timeline/model.hpp"

#include <array>
#include <cstdint>
#include <map>
#include <vector>

namespace canvas::core {

// Streaming 6-band parametric EQ for one clip. Reconfigure on any clip change.
// A mid-stream band edit — an Inspector EQ drag while playback streams — is
// made click-free two ways at once:
//
//   1. configure() carries the existing DF2T delay state across the change
//      instead of starting a cold zero filter (a cold state makes the first
//      filtered sample jump to ~b0*x, a full-scale pop per dragged commit).
//   2. Coeffient changes GLIDE from the current cascade to the new one over
//      kGlideFrames instead of snapping (a hard swap steps the b-coefficients
//      that scale x directly, even with the state carried). The filter is a
//      single variable-'b' cascade, so the glide keeps sample 0 continuous.
//
// reset() is still the hard stop for genuinely discontinuous audio
// (seek/rewind): it clears both the delay state and any pending glide.
class ParametricEqualizer final {
public:
    // One normalized biquad (a0 normalized to 1.0), produced by the shared
    // RBJ cookbook builder. Public only so equalizer_response() and the .cpp
    // cascade helpers can use the identical coefficient type.
    struct Coeff {
        double b0, b1, b2, a1, a2;  // normalized: a0 == 1.0
    };

    // Build/rebuild the cascade from the clip's six bands at `sample_rate`
    // (Hz) for `channels` (>= 1). Returns true when at least one band actually
    // filters (a fully-flat EQ configures to false and process() is a no-op,
    // which is the bit-exact pass-through path).
    [[nodiscard]] bool configure(int sample_rate, int channels,
                                 const std::array<Clip::EqBand, 6>& bands);

    [[nodiscard]] bool active() const;  // any band filters (configure() == true)

    // Filter `num_frames` interleaved frames in place (samples must hold
    // num_frames * channels floats). Writes exactly num_frames frames — the EQ
    // is a pure stream-in-place filter with no lookahead or priming.
    void process(float* samples, int num_frames);

    // Clear the IIR delay state (keeps the coefficients). Use on discontinuous
    // audio (seek/rewind) so no stale filter memory bleeds across the jump.
    void reset();

    // Length of the coefficient glide on a mid-stream reconfigure, in output
    // frames (2.7 ms at 48 kHz, ~64 frames into a 1600-frame mix chunk). Long
    // enough to kill the b-coefficient step, short enough that a fast drag
    // tracks the curve live instead of lagging behind the pointer.
    static constexpr int kGlideFrames = 128;

private:
    std::vector<Coeff> coeffs_;
    int channels_ = 0;
    std::vector<float> z1_;  // [stage * channels + channel]
    std::vector<float> z2_;
    std::vector<Coeff> glide_from_;  // cascade at the last reconfigure
    std::vector<Coeff> glide_to_;    // cascade being glided toward
    int glide_left_ = 0;             // frames remaining in the glide
};

// Magnitude response of the full 6-band cascade at `frequency` Hz, in dB
// (20*log10). Uses exactly the same band-selection + coefficient math as
// configure()/process(), so the Inspector's response plot and the audited DSP
// always agree. `frequency` is clamped into (0, 0.499*rate].
[[nodiscard]] double equalizer_response(const std::array<Clip::EqBand, 6>& bands,
                                        int sample_rate, double frequency);

// Per-clip registry so the realtime and export paths run the same equalizer
// continuously across calls (keyed by ClipId). Mirrors VoiceIsolationBank's
// lifecycle: drop() must be called when the timeline position jumps out of
// sequence (seek/rewind), and clear() on full reset.
class EqualizerBank final {
public:
    // Apply `bands` to `samples` (num_frames interleaved frames) in place for
    // `clip`. Returns num_frames. When `enabled` is false the clip passes
    // through untouched and any per-clip filter state is dropped (so toggling
    // EQ on later restarts from a fresh curve, exactly like the RNNoise bank).
    // `samples` may be null only when `enabled` is false (state-drop call).
    //
    // The filter is re-configured whenever `bands`, `sample_rate`, OR
    // `channels` change for a clip. The playback mix can legitimately flip a
    // clip's channel count over its life (the WSOLA pan path up-mixes a mono
    // source to a stereo front pair while pan != 0, then reverts to the
    // decoder's mono when pan is reset, with identical bands on both sides) —
    // process() strides the buffer by its CACHED channel count, so keying
    // reconfigure on bands alone would walk a shorter buffer with stale
    // geometry and read/write past its end.
    [[nodiscard]] int tick(std::uint64_t clip_id, const std::array<Clip::EqBand, 6>& bands,
                           bool enabled, int sample_rate, int channels, float* samples,
                           int num_frames);

    void drop();                     // reset all per-clip IIR state
    void drop(std::uint64_t clip_id);  // reset one clip's filter state
    void clear();                    // release every per-clip filter

private:
    struct Entry {
        ParametricEqualizer eq;
        std::array<Clip::EqBand, 6> bands{};
        int sample_rate = 0;
        int channels = 0;
        bool enabled = false;
    };
    std::map<std::uint64_t, Entry> entries_;
};

}  // namespace canvas::core