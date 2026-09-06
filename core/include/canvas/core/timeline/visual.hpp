#pragma once

// Shared ranges for the per-clip visual transform + composite properties. The
// limits mirror the reference app's numeric fields (Zoom 0..10, Position/Anchor
// -4096..4096 in pixels, Rotation -360..360 degrees, Opacity 0..1). Centralized
// here (Qt-free) so the edit ops clamp to the same bounds the Inspector's spin
// boxes expose — the UI never hardcodes a divergent set.
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

inline constexpr int kBlendModeCount = 5;  // Normal, Add, Multiply, Screen, Overlay

}  // namespace canvas::core::visual