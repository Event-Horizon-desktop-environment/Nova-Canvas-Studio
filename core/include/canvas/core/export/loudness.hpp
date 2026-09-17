#pragma once

// Loudness normalization LAW (Qt-free), backing the Deliver panel's
// `normalize_audio` / `normalize_target_lufs` settings.
//
// Two pieces:
//  1. `normalization_gain_db()` — the exposure gain that moves a measured
//     integrated loudness to the target, clamped to the shared audio_mix volume
//     bounds (so a normalization gain can never exceed a clip's own volume law).
//  2. `integrated_loudness_lufs()` — a gated block-RMS loudness estimator.
//
// NOTE on (2): this is an R128-FLAVOURED approximation. It uses 400 ms blocks
// with a 100 ms hop, a -70 LUFS absolute gate and a -10 LU relative gate (the
// EBU/ITU gating structure), but it does NOT yet apply the BS.1770 K-weighting
// filters or surround channel weights, and it omits the -0.691 calibration
// offset. For the common mono/steady material it is exercised against, the
// result equals the block RMS level in dBFS. True K-weighting is a follow-up
// subtask on this same module, keeping the estimator headless/tested.

#include <span>

namespace canvas::core::loudness {

// Loudness reported for digital silence: the absolute gate floor. A silent
// measurement therefore normalizes as +70 dB before clamping.
inline constexpr float kSilenceLufs = -70.0f;

// Gain (dB) to apply to move `measured_lufs` to `target_lufs`, clamped to the
// audio_mix volume law. Non-finite input (silence represented as -inf) maps to
// the maximum allowed boost.
[[nodiscard]] float normalization_gain_db(float measured_lufs, float target_lufs) noexcept;

// Integrated loudness of a mono PCM span (samples in [-1, 1]) in LUFS-ish dBFS.
// Returns kSilenceLufs when the span is empty, the rate is invalid, or every
// block falls below the absolute gate.
[[nodiscard]] float integrated_loudness_lufs(std::span<const float> mono,
                                             double sample_rate) noexcept;

}  // namespace canvas::core::loudness