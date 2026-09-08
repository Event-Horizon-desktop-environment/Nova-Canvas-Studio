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
                      const std::vector<float>& src, int src_ch,
                      const std::vector<float>* gains, float vol, float gl,
                      float gr) {
    if (out_channels <= 0 || src_ch <= 0 || src.empty() || out.empty()) return;
    const int frames = static_cast<int>(src.size()) / src_ch;
    if (frames <= 0) return;
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

}  // namespace audio_mix

}  // namespace canvas::core