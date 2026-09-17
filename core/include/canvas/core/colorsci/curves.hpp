#pragma once

#include <array>
#include <cmath>
#include <utility>
#include <vector>

#include "canvas/core/colorsci/wheels.hpp"

namespace canvas::core::colorsci {

inline constexpr float kLuma601R = 0.299f;
inline constexpr float kLuma601G = 0.587f;
inline constexpr float kLuma601B = 0.114f;

enum class CurveChannel {
    kLuma = 0,
    kRed = 1,
    kGreen = 2,
    kBlue = 3,
    kCount = 4,
};

inline constexpr int kCurveChannelCount = static_cast<int>(CurveChannel::kCount);

struct CurvePoint {
    float x = 0.0f;
    float y = 0.0f;
};

inline bool operator==(const CurvePoint& a, const CurvePoint& b) noexcept {
    return a.x == b.x && a.y == b.y;
}

struct SoftClip {
    float low = 0.0f;
    float low_soft = 0.0f;
    float high = 1.0f;
    float high_soft = 0.0f;

    [[nodiscard]] bool is_identity() const noexcept {
        return low <= 0.0f && low_soft <= 0.0f && high >= 1.0f && high_soft <= 0.0f;
    }
};

struct CurveParams {
    std::array<std::vector<CurvePoint>, kCurveChannelCount> channels{};
    SoftClip soft_clip{};

    [[nodiscard]] bool is_identity() const noexcept {
        if (!soft_clip.is_identity()) return false;
        for (const auto& ch : channels) {
            if (!ch.empty()) return false;
        }
        return true;
    }
};

[[nodiscard]] float eval_curve(const std::vector<CurvePoint>& points, float x);

[[nodiscard]] float eval_soft_clip_high(float x, float high, float soft) noexcept;

[[nodiscard]] float eval_soft_clip_low(float x, float low, float soft) noexcept;

[[nodiscard]] RGBF apply_curves(const RGBF& rgb, const CurveParams& c) noexcept;

}