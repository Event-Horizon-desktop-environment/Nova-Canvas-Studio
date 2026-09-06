#pragma once

// Shared ranges for the per-clip audio processing properties (pitch, speed,
// EQ). Centralized here (Qt-free) so the edit ops clamp to the same bounds
// the Inspector's spin boxes expose — the UI never hardcodes a divergent set.
namespace canvas::core::audio_processing {

inline constexpr float kPitchSemitonesMin = -12.0f;
inline constexpr float kPitchSemitonesMax = 12.0f;
inline constexpr float kPitchSemitonesDefault = 0.0f;

inline constexpr float kPitchCentsMin = -100.0f;
inline constexpr float kPitchCentsMax = 100.0f;
inline constexpr float kPitchCentsDefault = 0.0f;

inline constexpr float kSpeedMin = 0.1f;
inline constexpr float kSpeedMax = 10.0f;
inline constexpr float kSpeedDefault = 1.0f;

inline constexpr float kEqFreqMin = 20.0f;
inline constexpr float kEqFreqMax = 20000.0f;
inline constexpr float kEqFreqDefault = 1000.0f;

inline constexpr float kEqGainMin = -24.0f;
inline constexpr float kEqGainMax = 24.0f;
inline constexpr float kEqGainDefault = 0.0f;

inline constexpr float kEqQMin = 0.1f;
inline constexpr float kEqQMax = 10.0f;
inline constexpr float kEqQDefault = 1.0f;

inline constexpr int kEqBandCount = 6;

inline constexpr float kTransitionCurveMin = 0.0f;
inline constexpr float kTransitionCurveMax = 1.0f;
inline constexpr float kTransitionCurveDefault = 0.0f;

inline constexpr float kTransitionRatioMin = 0.0f;
inline constexpr float kTransitionRatioMax = 100.0f;

}  // namespace canvas::core::audio_processing
