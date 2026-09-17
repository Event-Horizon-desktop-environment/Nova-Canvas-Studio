#pragma once

#include <algorithm>
#include <cmath>
#include <numbers>

namespace canvas::core::colorsci {

struct RGBF {
    float r = 0.0f;
    float g = 0.0f;
    float b = 0.0f;
};

inline constexpr float kLumaR = 0.2126f;
inline constexpr float kLumaG = 0.7152f;
inline constexpr float kLumaB = 0.0722f;

inline constexpr float kLiftLo = -1.0f;
inline constexpr float kLiftHi = 1.0f;
inline constexpr float kGammaLo = 0.25f;
inline constexpr float kGammaHi = 4.0f;
inline constexpr float kGainLo = 0.0f;
inline constexpr float kGainHi = 2.0f;
inline constexpr float kOffsetLo = -1.0f;
inline constexpr float kOffsetHi = 1.0f;
inline constexpr float kContrastLo = 0.0f;
inline constexpr float kContrastHi = 4.0f;
inline constexpr float kPivotLo = 0.0f;
inline constexpr float kPivotHi = 1.0f;
inline constexpr float kSatLo = 0.0f;
inline constexpr float kSatHi = 2.0f;
inline constexpr float kHueDegLo = -180.0f;
inline constexpr float kHueDegHi = 180.0f;
inline constexpr float kColorBoostLo = 0.0f;
inline constexpr float kColorBoostHi = 2.0f;
inline constexpr float kLumMixLo = 0.0f;
inline constexpr float kLumMixHi = 1.0f;
inline constexpr float kZoneSatLo = -1.0f;
inline constexpr float kZoneSatHi = 1.0f;
inline constexpr float kDefaultPivot = 0.435f;

[[nodiscard]] inline float luma(const RGBF& c) noexcept {
    return kLumaR * c.r + kLumaG * c.g + kLumaB * c.b;
}

[[nodiscard]] inline float clamp01(float v) noexcept {
    return std::clamp(v, 0.0f, 1.0f);
}

[[nodiscard]] inline float lift_gamma_gain_channel(float in, float lift, float gamma, float gain) {
    const float base = gain * (in + lift * (1.0f - in));
    if (base <= 0.0f) return 0.0f;
    const float g = std::clamp(gamma, 0.1f, 10.0f);
    return std::pow(base, 1.0f / g);
}

struct LGG {
    float lift_master = 0.0f;
    float gamma_master = 1.0f;
    float gain_master = 1.0f;
    float lift_r = 0.0f, lift_g = 0.0f, lift_b = 0.0f;
    float gamma_r = 1.0f, gamma_g = 1.0f, gamma_b = 1.0f;
    float gain_r = 1.0f, gain_g = 1.0f, gain_b = 1.0f;
};

[[nodiscard]] inline RGBF apply_lgg(const RGBF& in, const LGG& p) noexcept {
    RGBF out;
    out.r = lift_gamma_gain_channel(in.r, p.lift_master + p.lift_r,
                                    p.gamma_master * p.gamma_r, p.gain_master * p.gain_r);
    out.g = lift_gamma_gain_channel(in.g, p.lift_master + p.lift_g,
                                    p.gamma_master * p.gamma_g, p.gain_master * p.gain_g);
    out.b = lift_gamma_gain_channel(in.b, p.lift_master + p.lift_b,
                                    p.gamma_master * p.gamma_b, p.gain_master * p.gain_b);
    return out;
}

struct Offset {
    float master = 0.0f;
    float r = 0.0f, g = 0.0f, b = 0.0f;
};

[[nodiscard]] inline RGBF apply_offset(const RGBF& in, const Offset& o) noexcept {
    const float m = o.master;
    return RGBF{in.r + m + o.r, in.g + m + o.g, in.b + m + o.b};
}

[[nodiscard]] inline RGBF apply_contrast_pivot(const RGBF& in, float contrast,
                                               float pivot) noexcept {
    return RGBF{pivot + (in.r - pivot) * contrast, pivot + (in.g - pivot) * contrast,
                pivot + (in.b - pivot) * contrast};
}

[[nodiscard]] inline RGBF apply_saturation(const RGBF& in, float sat) noexcept {
    const float L = luma(in);
    return RGBF{L + (in.r - L) * sat, L + (in.g - L) * sat, L + (in.b - L) * sat};
}

[[nodiscard]] inline RGBF apply_color_boost(const RGBF& in, float boost) noexcept {
    const float mx = std::max(in.r, std::max(in.g, in.b));
    const float mn = std::min(in.r, std::min(in.g, in.b));
    const float s = clamp01(mx - mn);
    const float factor = std::clamp(1.0f + boost * (1.0f - s), 0.0f, kSatHi);
    return apply_saturation(in, factor);
}

[[nodiscard]] inline RGBF apply_hue_rotate(const RGBF& in, float degrees) noexcept {
    const float Y = luma(in);
    const float cr = kLumaR * (in.r - Y);
    const float cb = kLumaB * (in.b - Y);
    const float th = degrees * std::numbers::pi_v<float> / 180.0f;
    const float c = std::cos(th);
    const float s = std::sin(th);
    const float cr2 = cr * c - cb * s;
    const float cb2 = cr * s + cb * c;
    const float r = Y + cr2 / kLumaR;
    const float b = Y + cb2 / kLumaB;
    const float g = Y - (cr2 + cb2) / kLumaG;
    return RGBF{r, g, b};
}

inline constexpr float kShadowZoneHi = 0.4f;
inline constexpr float kHighlightZoneLo = 0.6f;

[[nodiscard]] inline float smoothstep01(float edge0, float edge1, float x) noexcept {
    const float t = clamp01((x - edge0) / (edge1 - edge0));
    return t * t * (3.0f - 2.0f * t);
}

[[nodiscard]] inline float shadow_weight(float luma_val) noexcept {
    return 1.0f - smoothstep01(0.0f, kShadowZoneHi, luma_val);
}

[[nodiscard]] inline float highlight_weight(float luma_val) noexcept {
    return smoothstep01(kHighlightZoneLo, 1.0f, luma_val);
}

[[nodiscard]] inline RGBF apply_saturation_zoned(const RGBF& in, float sat,
                                                 float shadow_amt, float highlight_amt) noexcept {
    const float L = luma(in);
    const float w_sh = shadow_weight(L);
    const float w_hi = highlight_weight(L);
    const float eff = std::clamp(sat * (1.0f + shadow_amt * w_sh + highlight_amt * w_hi),
                                 0.0f, kSatHi);
    return apply_saturation(in, eff);
}

[[nodiscard]] inline RGBF apply_lum_mix(const RGBF& corrected, const RGBF& original,
                                        float lum_mix) noexcept {
    const float dl = luma(corrected) - luma(original);
    RGBF luma_only{original.r + dl, original.g + dl, original.b + dl};
    return RGBF{luma_only.r + (corrected.r - luma_only.r) * lum_mix,
                luma_only.g + (corrected.g - luma_only.g) * lum_mix,
                luma_only.b + (corrected.b - luma_only.b) * lum_mix};
}

[[nodiscard]] inline RGBF hue_to_rgb(float degrees) noexcept {
    const float th = degrees * std::numbers::pi_v<float> / 180.0f;
    const float c2 = std::numbers::pi_v<float> * 2.0f / 3.0f;
    return RGBF{std::cos(th), std::cos(th - c2), std::cos(th - 2.0f * c2)};
}

[[nodiscard]] inline RGBF puck_xy_to_offset(float dx, float dy) noexcept {
    const float radius = std::clamp(std::hypot(dx, dy), 0.0f, 1.0f);
    if (radius <= 0.0f) return RGBF{0.0f, 0.0f, 0.0f};
    const float deg = std::atan2(dy, dx) * 180.0f / std::numbers::pi_v<float>;
    const RGBF hue = hue_to_rgb(deg);
    return RGBF{hue.r * radius, hue.g * radius, hue.b * radius};
}

}