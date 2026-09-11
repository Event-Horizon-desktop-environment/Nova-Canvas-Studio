#pragma once

// Per-clip audio-volume/pan helpers (Qt-free, core domain). Shared by live
// playback (AudioPipeline), the export renderer, and the Inspector so the
// custom clip gain law is identical everywhere.
//
// Volume is stored as decibels (0 dB = unity) and converted with
// gain = 10^(db/20). Pan is stored in [-1,1] and rendered with a linear stereo
// BALANCE law: pan 0.0 leaves both channels at unity (the original signal), and
// moving the control rides the OPPOSITE channel down so a hard-panned clip
// still plays at full level on its side.

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <vector>

namespace canvas::core {

namespace audio_mix {
// Audible gain law: everything at/below the floor is silenced by db_to_gain (a
// clip dragged to the very bottom of the timeline IS digital silence, gain -> 0)
// and the ceiling caps the boost, so a clip can never leave the clinically-safe
// band during playback/export no matter what the control stores. The floor
// matches the Inspector slider's minimum (-100 dB), so the timeline line, the
// Inspector, playback and export all agree on where silence lives.
constexpr float kMinVolumeDb = -100.0f;
constexpr float kMaxVolumeDb = 24.0f;
// The Inspector's volume control range, wider than the law on purpose at the TOP
// only: a symmetric [-100, +100] dB slider puts 0 dB exactly at center. Values
// above the ceiling (+24) are accepted by edit_ops and stored on the clip (no
// UI snap-back), but db_to_gain clamps them into the law band on render.
constexpr float kVolumeDbSliderMin = -100.0f;
constexpr float kVolumeDbSliderMax = 100.0f;
constexpr float kPanMin = -1.0f;
constexpr float kPanMax = 1.0f;

[[nodiscard]] constexpr float normalize_volume_db(float db) noexcept {
    return std::clamp(db, kMinVolumeDb, kMaxVolumeDb);
}

[[nodiscard]] constexpr float normalize_pan(float pan) noexcept {
    return std::clamp(pan, kPanMin, kPanMax);
}

// dBFS gain to a linear multiplier (1.0 right at 0 dB).
[[nodiscard]] inline float db_to_gain(float db) noexcept {
    const float v = normalize_volume_db(db);
    if (v <= kMinVolumeDb) return 0.0f;
    return std::pow(10.0f, v / 20.0f);
}

// Stereo BALANCE gains for a per-clip pan control, mirroring the reference NLE
// behavior implied by "Pan: 0.0" = center with the track unchanged. Linear in
// gain: pan -1 = hard left (L unity, R silent), 0 = center (both unity, the
// original signal untouched), +1 = hard right. A centered clip is therefore
// bit-for-bit the decoded PCM at unity, and moving the pan only rides the
// OPPOSITE channel down — no power-boost on the side the signal is pushed
// toward.
inline void pan_gains(float pan, float& left, float& right) noexcept {
    const float p = normalize_pan(pan);
    if (p <= 0.0f) {
        left = 1.0f;
        right = 1.0f + p;  // [-1, 0] -> [0, 1]
    } else {
        left = 1.0f - p;   // (0, 1] -> [1, 0)
        right = 1.0f;
    }
}

// Whether any track in `tracks` is soloed (the trigger for solo isolation).
// Qt-free; the caller picks the sequence's audio track range.
template <typename Container>
[[nodiscard]] inline bool any_solo(const Container& tracks) noexcept {
    for (const auto& t : tracks)
        if (t.solo) return true;
    return false;
}

// Adds one interleaved source chunk into `out`, a zero-initialised accumulator
// of [frames x out_channels] floats. Applies the per-frame fade envelope
// `gains` (nullptr = unity), the clip volume `vol`, and the track balance
// `gl`/`gr`. Mono sources are upmixed to the front pair (both channels), a
// >2ch surround source folds down for a 2-channel bus (rear/side channels at
// half gain, paired by side) or maps channel c -> min(c, out_channels - 1)
// for wider buses, and stereo sources stay on the front pair. Shared by
// playback and export so the two mix identically at any channel count.
inline void mix_chunk(std::vector<float>& out, int out_channels,
                      const float* src, int src_ch, int frames,
                      const std::vector<float>* gains, float vol, float gl,
                      float gr) {
    if (out_channels <= 0 || src_ch <= 0 || frames <= 0 || !src || out.empty()) return;
    const std::size_t row = static_cast<std::size_t>(out_channels);
    for (int k = 0; k < frames; ++k) {
        const float g = gains ? (*gains)[static_cast<std::size_t>(k)] : 1.0f;
        const float base = g * vol;
        const std::size_t base_idx = static_cast<std::size_t>(k) * row;
        if (src_ch == 1) {
            const float v = src[static_cast<std::size_t>(k)];
            out[base_idx] += v * base * (out_channels > 1 ? gl : 1.0f);
            if (out_channels > 1) out[base_idx + 1] += v * base * gr;
        } else if (src_ch == 2) {
            const float l = src[static_cast<std::size_t>(k) * 2];
            const float r = src[static_cast<std::size_t>(k) * 2 + 1];
            out[base_idx] += l * base * (out_channels > 1 ? gl : 1.0f);
            if (out_channels > 1) out[base_idx + 1] += r * base * gr;
        } else if (out_channels == 2) {
            const std::size_t so = static_cast<std::size_t>(k) * static_cast<std::size_t>(src_ch);
            out[base_idx] += src[so] * base * gl;
            out[base_idx + 1] += src[so + 1] * base * gr;
            for (int c = 2; c < src_ch; ++c) {
                const float v = src[so + static_cast<std::size_t>(c)] * base * 0.5f;
                if (c % 2 == 0) out[base_idx] += v * gl;
                else out[base_idx + 1] += v * gr;
            }
        } else {
            const std::size_t so = static_cast<std::size_t>(k) * static_cast<std::size_t>(src_ch);
            for (int c = 0; c < src_ch; ++c) {
                const std::size_t dst =
                    c < out_channels ? static_cast<std::size_t>(c) : row - 1;
                out[base_idx + dst] += src[so + static_cast<std::size_t>(c)] * base;
            }
        }
    }
}

// Vector convenience overload (the pre-existing surface). The pointer+frames
// form above is the single shared law so the per-clip AI voice-isolation stage
// (which hands the mix a possibly-shorter denoised buffer) cannot drift from
// the export/playback channel-mapping math.
inline void mix_chunk(std::vector<float>& out, int out_channels,
                      const std::vector<float>& src, int src_ch,
                      const std::vector<float>* gains, float vol, float gl,
                      float gr) {
    if (out_channels <= 0 || src_ch <= 0 || src.empty() || out.empty()) return;
    mix_chunk(out, out_channels, src.data(), src_ch,
              static_cast<int>(src.size()) / src_ch, gains, vol, gl, gr);
}

// --- Master-bus loudness ceiling (playback AND export share this law) ---
//
// Extreme per-clip EQ curves (the field profile from project nv.ehproj: a
// 20 Hz low-shelf +18.1 dB, bell 57 Hz +18.1 dB, bell 97 Hz +10.5 dB — its
// near-unit-circle poles measure ~13x gain at 60 Hz) and hot multi-lane sums
// routinely exceed 0 dBFS on the float bus. Without a bound that excess is
// handed to the DAC, which hard-clips it into the ±1.0 Nyquist square bursts
// a recording captures (measured 2026-09-11 on 2026-09-11-18-25-46.mkv). The
// limiter is instant-attack (a sample that would exceed the ceiling is
// attenuated the very same sample by exactly the ratio it exceeded), slow
// release (the gain recovers toward unity at kMasterReleasePerSample each
// following sample), so loud program never overflows the DAC while the mix
// stays continuous and click-free. reset() on seek/rewind so a held gain drop
// cannot bleed across the jump.
constexpr float kMasterCeiling = 0.98f;                   // DAC-safe peak (< 0 dBFS)
constexpr float kMasterReleasePerSample = 1.0f / 4096.0f;  // ~85 ms time constant @ 48 kHz
// A single clip's lane may legitimately sum loud (music + voice at +24 dB on
// one lane) but values far above unity cannot come from valid program: the
// pipeline's own audit calls >16 "BLAST" and caught float32 monsters reading
// 1e10..1e20 (bit-exact "data"/0x61746164 = non-audio bytes read as PCM) at
// DSP seam spikes. kLaneSanityCeiling is the clamp line for a lane held
// before it mixes.
constexpr float kLaneSanityCeiling = 4.0f;

struct MasterLimiter {
    float gain_ = 1.0f;
    // Return the gain to apply to `sample` (instant attack, slow release).
    [[nodiscard]] float gain_for(float sample) noexcept {
        const float peak = std::fabs(sample);
        const float target = peak > kMasterCeiling ? (kMasterCeiling / peak) : 1.0f;
        if (target < gain_) gain_ = target;  // instant attack: this sample is tamed
        else if (gain_ < 1.0f) gain_ = std::min(1.0f, gain_ + kMasterReleasePerSample);
        return gain_;
    }
    // Apply to one bus sample and HARD-CLAMP the product to the ceiling. The
    // raw multiply can round a hair above kMasterCeiling in float32 (measured
    // 0.9809 vs 0.98 on the field tone — release-path f32 rounding), which is
    // still below the 1.0 DAC clip line but would trip a strict ceiling
    // assertion; the clamp makes the peak bound exact.
    [[nodiscard]] float apply(float sample) noexcept {
        float out = sample * gain_for(sample);
        if (std::fabs(out) > kMasterCeiling) out = std::copysign(kMasterCeiling, out);
        return out;
    }
    void reset() noexcept { gain_ = 1.0f; }
};

// Replace non-finite, or |v| > kLaneSanityCeiling, samples in one interleaved
// lane with the last valid sample (hold-to-valid — the same law as the
// decoder corrupt-head repair, generalized to any DSP stage). A float32
// monster in a single clip's DSP can therefore never reach the DAC as a
// clipped square. `samples` is modified in place; returns the number of
// samples held.
inline int sanitize_lane(float* samples, int frames, int channels) noexcept {
    if (!samples || frames <= 0 || channels <= 0) return 0;
    int held = 0;
    for (int c = 0; c < channels; ++c) {
        float last = 0.0f;
        for (int k = 0; k < frames; ++k) {
            float& v = samples[static_cast<std::size_t>(k) * static_cast<std::size_t>(channels) +
                               static_cast<std::size_t>(c)];
            if (std::isfinite(v) && std::fabs(v) <= kLaneSanityCeiling) {
                last = v;
            } else {
                v = last;
                ++held;
            }
        }
    }
    return held;
}

}  // namespace audio_mix

}  // namespace canvas::core