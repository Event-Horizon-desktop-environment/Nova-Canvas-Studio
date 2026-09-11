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
//  - Bell/Shelf bands with |gain| <= 1e-3 dB are skipped — their cascade slot
//    becomes an EXACT identity biquad (b0=1, b1=b2=a1=a2=0) rather than being
//    dropped, so a flat EQ is a bit-exact pass-through while the cascade stays
//    a constant six stages (slot i always holds band i). LowPass/HighPass/
//    Notch always filter. Band frequencies above 0.45*rate are clamped into
//    the valid range so no coefficient degenerates near Nyquist (an export at
//    a low rate with a 20 kHz band stays stable).
//
// A constant slot-stable cascade is what makes every mid-stream transition
// click-free at the FILTER level: bypassing a band, dragging a Bell/Shelf gain
// across the 1e-3 identity threshold, or switching a band's type never changes
// the stage count, so the coefficient glide below always applies and DF2T
// state always belongs to the same band across a reconfigure.

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
//   1. The cascade is SLOT-STABLE: slot i always holds band i's biquad
//      (identity biquads fill slots whose band doesn't shape). A bypass/
//      enable, a type change, or a gain crossing the 1e-3 identity threshold
//      changes a slot's coefficients but NEVER the stage count or a slot's
//      band identity — so DF2T state is carried per band (never misaligned
//      onto the wrong biquad) and the coefficient glide below always applies.
//   2. Coefficient changes GLIDE from the current cascade to the new one over
//      kGlideFrames instead of snapping (a hard swap steps the b-coefficients
//      that scale x directly, even with the state carried). The filter is a
//      single variable-'b' cascade, so the glide keeps sample 0 continuous.
//
// State carry per slot is gated on "that band was actually filtering at the
// last configure": a slot that is entering active service cold-starts (its
// coefficients glide in from identity, so the fade-in is clean); a slot being
// retired keeps its state and glides toward identity, draining the transient
// without a step. reset() is still the hard stop for genuinely discontinuous
// audio (seek/rewind): it clears both the delay state and any pending glide.
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
    // filters (a fully-flat EQ configures to false, which is the bit-exact
    // pass-through path). Every configure() after the first GLIDES to the new
    // curve (the stage count is constant); configure() returns false only for
    // a fully-flat configuration, which process() renders exactly.
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
    // Slot-stable state-carve bookkeeping: which bands were actually filtering
    // at the LAST configure (so the carry knows a slot that is entering active
    // service must cold-start, and a slot being retired keeps its state to
    // drain through the glide), and the channel count the carried state lives
    // in (a mono<->stereo flip re-indexes the carry correctly).
    std::array<bool, 6> prev_active_{};
    int prev_channels_ = 1;
    bool any_active_ = false;
};

// Magnitude response of the full 6-band cascade at `frequency` Hz, in dB
// (20*log10). Uses exactly the same band-selection + coefficient math as
// configure()/process(), so the Inspector's response plot and the audited DSP
// always agree. `frequency` is clamped into (0, 0.499*rate].
[[nodiscard]] double equalizer_response(const std::array<Clip::EqBand, 6>& bands,
                                        int sample_rate, double frequency);

// Magnitude response of ONE band's biquad at `frequency` Hz, in dB — the
// per-band contribution to the cascade. Same coefficient law as
// equalizer_response() (and the same band_filters() selection in the composite
// version), so summing the curves the per-band value maps out across the grid
// reconstructs the composite plot. Disabled bands are exactly 0 dB. Powers the
// Inspector's per-band translucent fills (FreeEQ8-style band curves).
[[nodiscard]] double equalizer_band_response(const Clip::EqBand& band, int sample_rate,
                                             double frequency);

// Per-clip registry so the realtime and export paths run the same equalizer
// continuously across calls (keyed by ClipId). Mirrors VoiceIsolationBank's
// lifecycle: drop() must be called when the timeline position jumps out of
// sequence (seek/rewind), and clear() on full reset.
class EqualizerBank final {
public:
    // Apply `bands` to `samples` (num_frames interleaved frames) in place for
    // `clip`. Returns num_frames. When `enabled` is false the clip passes
    // through untouched BUT the per-clip filter state is KEPT: toggling EQ off
    // and back on resumes the filter from its carried DF2T state instead of
    // cold-starting a zero-state filter (this mirrors FreeEQ8, which early-
    // returns on a disabled band and only reset()/prepareToPlay clears state).
    //
    // Toggle transitions are click-free at the OUTPUT level, not just the
    // filter level: the single-sample snap between the shaped stream and the
    // raw PCM (a measured ~0.6-1.3 step on band-center content) survives any
    // amount of filter-state carry, so tick() crossfades the filter's
    // contribution over kGlideFrames on BOTH edges — dry->wet on enable,
    // wet->dry on disable — by blending `wet*processed + (1-wet)*raw` per
    // frame while feeding the filter the whole time (the filter only consumes
    // input during the glide; a settled-disabled clip is a zero-cost
    // pass-through and stays byte-exact). `enabled` drives a per-entry wet
    // mix: 1.0 fully processed, 0.0 dry. drop()/drop(clip_id) snap the mix to
    // its settled state as part of the discontinuous-audio clear (seek/rewind).
    //
    // `samples` may be null only when `enabled` is false AND tick() was told
    // nothing is gliding — call wants_samples() first so a disable edge mid-
    // glide still receives real PCM (the pipeline does this: it feeds while
    // wants_samples() is true, and hands null/0 once the clip settles dry). A
    // null call on a mid-glide entry cannot blend, so it snaps the mix dry.
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

    // Whether tick() for `clip` wants real PCM this call: the clip is
    // `enabled`, or it is disabled but still mid-glide toward dry (an entry
    // with wet > 0 that has not reached its settled pass-through yet). Cheap
    // query so callers avoid a copy for ever-disabled clips.
    [[nodiscard]] bool wants_samples(std::uint64_t clip_id, bool enabled) const;

    void drop();                     // reset all per-clip IIR state
    void drop(std::uint64_t clip_id);  // reset one clip's filter state
    void clear();                    // release every per-clip filter

private:
    struct Entry {
        ParametricEqualizer eq;
        std::array<Clip::EqBand, 6> bands{};
        int sample_rate = 0;
        int channels = 0;
        bool enabled = false;      // last tick() enable state, for toggle logging
        float wet_ = 0.0f;         // current dry/wet mix: 1.0 = fully processed
        std::vector<float> scratch_ = {};  // raw input snapshot during a glide
    };
    // Blend `entry` from its current wet_ toward `target` (1.0 enabled, 0.0
    // disabled) over kGlideFrames of OUTPUT frames: `samples` already holds the
    // filter's processed output for this call, `entry.scratch_` the raw input,
    // so each frame writes `wet*processed + (1-wet)*raw` while stepping wet one
    // kGlideFrame-th toward the target per frame. Called exactly on the two
    // toggle edges; steady-state ticks never blend.
    void glide_to(Entry& entry, float target, float* samples, int channels, int num_frames);

    std::map<std::uint64_t, Entry> entries_;
};

}  // namespace canvas::core