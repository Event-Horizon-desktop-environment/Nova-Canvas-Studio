#pragma once

#include "canvas/core/colorsci/cdl.hpp"
#include "canvas/core/colorsci/wheels.hpp"

#include <array>
#include <cstddef>

namespace canvas::core::colorsci {

enum class WheelPage { kPrimaries, kLog, kHdr, kCdl };

constexpr int kWheelPageCount = 4;

[[nodiscard]] WheelPage wheel_page_next(WheelPage p) noexcept;
[[nodiscard]] WheelPage wheel_page_prev(WheelPage p) noexcept;
[[nodiscard]] WheelPage clamp_wheel_page(int index) noexcept;
[[nodiscard]] const char* wheel_page_title(WheelPage p) noexcept;

[[nodiscard]] inline RGBF scaled_wheel_offset(float dx, float dy, float scale) noexcept {
    const RGBF off = puck_xy_to_offset(dx, dy);
    return RGBF{off.r * scale, off.g * scale, off.b * scale};
}

inline constexpr float kWheelLiftScale = 0.20f;
inline constexpr float kWheelGammaScale = 2.0f;
inline constexpr float kWheelGainScale = 1.0f;
inline constexpr float kWheelOffsetScale = 0.12f;

[[nodiscard]] float master_to_value(float t01, float lo, float hi, float mid) noexcept;
[[nodiscard]] float value_to_master(float v, float lo, float hi, float mid) noexcept;

struct WheelRange {
    float lo = 0.0f;
    float hi = 1.0f;
};

struct LogWheelState {
    LGG lgg;
    WheelRange band{0.0f, 1.0f};
    bool uniform = false;
};

void enforce_zero_overlap(std::array<WheelRange, 4>& bands) noexcept;

[[nodiscard]] float log_band_weight(float luma, const WheelRange& band, bool uniform) noexcept;

struct HdrZone {
    float position = 0.5f;
    float falloff = 0.25f;
    float exp = 0.0f;
    float sat = 0.0f;
    RGBF offset;
    bool uniform = false;
};

inline constexpr int kHdrStockZones = 4;
inline constexpr int kHdrMaxZones = 12;

[[nodiscard]] float hdr_zone_weight(float luma, const HdrZone& z) noexcept;

[[nodiscard]] LGG lgg_from_cdl(const Cdl& c) noexcept;

enum class ToneParam : int {
    kTemp = 0,
    kTint,
    kHue,
    kContrast,
    kPivot,
    kMidDetail,
    kBlackOffset
};
inline constexpr int kToneParamCount = 7;

inline constexpr float kTempLo = -100.0f;
inline constexpr float kTempHi = 100.0f;
inline constexpr float kTintLo = -100.0f;
inline constexpr float kTintHi = 100.0f;
inline constexpr float kMidDetailLo = -100.0f;
inline constexpr float kMidDetailHi = 100.0f;

struct WheelPanelState {
    WheelPage page = WheelPage::kPrimaries;

    LGG lgg;
    Offset offset;

    float temp = 0.0f;
    float tint = 0.0f;
    float hue_deg = 0.0f;
    float contrast = 1.0f;
    float pivot = kDefaultPivot;
    float mid_detail = 0.0f;
    float black_offset = 0.0f;
    float color_boost = 0.0f;
    float shadows_sat = 0.0f;
    float highlights_sat = 0.0f;
    float saturation = 1.0f;
    float lum_mix = 1.0f;

    std::array<LogWheelState, 4> log{};

    std::array<HdrZone, kHdrStockZones> hdr{};

    Cdl cdl;
};

enum class PrimariesWheel : int { kLift = 0, kGamma = 1, kGain = 2, kOffset = 3 };
inline constexpr int kPrimariesWheelCount = 4;

namespace detail {
struct WheelRanges {
    float lo_master, hi_master, id_master;
    float scale;
};
inline constexpr WheelRanges kWheelMeta[4] = {
    {kLiftLo, kLiftHi, 0.0f, kWheelLiftScale},
    {kGammaLo, kGammaHi, 1.0f, kWheelGammaScale},
    {kGainLo, kGainHi, 1.0f, kWheelGainScale},
    {kOffsetLo, kOffsetHi, 0.0f, kWheelOffsetScale},
};
}

void apply_primaries_wheel(WheelPanelState& s, PrimariesWheel w, float dx, float dy,
                           float master01) noexcept;

void reset_primaries_wheel(WheelPanelState& s, PrimariesWheel w) noexcept;
void reset_panel(WheelPanelState& s) noexcept;

void restore_primaries_wheel(WheelPanelState& to, const WheelPanelState& from,
                             PrimariesWheel w) noexcept;

}
