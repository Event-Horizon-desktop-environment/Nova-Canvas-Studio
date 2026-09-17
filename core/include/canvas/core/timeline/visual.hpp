#pragma once

namespace canvas::core::visual {

inline constexpr float kScaleMin = 0.0f;
inline constexpr float kScaleMax = 10.0f;
inline constexpr float kScaleDefault = 1.0f;

inline constexpr double kPosMin = -4096.0;
inline constexpr double kPosMax = 4096.0;
inline constexpr double kPosDefault = 0.0;

inline constexpr float kRotationMin = -360.0f;
inline constexpr float kRotationMax = 360.0f;
inline constexpr float kRotationDefault = 0.0f;

inline constexpr double kAnchorMin = -4096.0;
inline constexpr double kAnchorMax = 4096.0;
inline constexpr double kAnchorDefault = 0.0;

inline constexpr float kOpacityMin = 0.0f;
inline constexpr float kOpacityMax = 1.0f;
inline constexpr float kOpacityDefault = 1.0f;

inline constexpr int kBlendModeCount = 8;

}
