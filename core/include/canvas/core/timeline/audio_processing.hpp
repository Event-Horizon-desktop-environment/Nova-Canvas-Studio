#pragma once

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
inline constexpr float kTransitionCurveOutDefault = 1.0f;

inline constexpr float kTransitionRatioMin = 0.0f;
inline constexpr float kTransitionRatioMax = 100.0f;
inline constexpr int kTransitionRatioStartDefault = 0;
inline constexpr int kTransitionRatioEndDefault = 100;

}
