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

namespace canvas::core {

namespace audio_mix {
// Audible gain law: everything at/below the floor is silenced by db_to_gain and
// the ceiling caps the boost, so a clip can never leave the clinically-safe
// band during playback/export no matter what the control stores.
constexpr float kMinVolumeDb = -60.0f;
constexpr float kMaxVolumeDb = 24.0f;
// The Inspector's volume control range, wider than the law on purpose: a
// symmetric [-100, +100] dB slider puts 0 dB exactly at center. Values outside
// [-60, +24] are accepted by edit_ops and stored on the clip (no UI snap-back),
// but db_to_gain clamps them into the law band on render.
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

}  // namespace audio_mix

}  // namespace canvas::core