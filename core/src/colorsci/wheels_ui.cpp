#include "canvas/core/colorsci/wheels_ui.hpp"

#include <algorithm>
#include <cmath>

namespace canvas::core::colorsci {

WheelPage wheel_page_next(WheelPage p) noexcept {
    return static_cast<WheelPage>((static_cast<int>(p) + 1) % kWheelPageCount);
}

WheelPage wheel_page_prev(WheelPage p) noexcept {
    return static_cast<WheelPage>((static_cast<int>(p) + kWheelPageCount - 1) % kWheelPageCount);
}

WheelPage clamp_wheel_page(int index) noexcept {
    return static_cast<WheelPage>(std::clamp(index, 0, kWheelPageCount - 1));
}

const char* wheel_page_title(WheelPage p) noexcept {
    switch (p) {
        case WheelPage::kPrimaries: return "Primaries";
        case WheelPage::kLog: return "Log";
        case WheelPage::kHdr: return "HDR";
        case WheelPage::kCdl: return "CDL";
    }
    return "Primaries";
}

float master_to_value(float t01, float lo, float hi, float mid) noexcept {
    const float t = std::clamp(t01, 0.0f, 1.0f);
    if (t <= 0.5f) {
        // [0, 0.5] maps lo -> mid.
        return lo + (mid - lo) * (t / 0.5f);
    }
    // [0.5, 1] maps mid -> hi.
    return mid + (hi - mid) * ((t - 0.5f) / 0.5f);
}

float value_to_master(float v, float lo, float hi, float mid) noexcept {
    const float x = std::clamp(v, lo, hi);
    if (x <= mid) {
        const float denom = mid - lo;
        return denom <= 0.0f ? 0.5f : 0.5f * ((x - lo) / denom);
    }
    const float denom = hi - mid;
    return denom <= 0.0f ? 0.5f : 0.5f + 0.5f * ((x - mid) / denom);
}

void enforce_zero_overlap(std::array<WheelRange, 4>& bands) noexcept {
    // Sort by low, then pin each band's low to the previous band's high so the
    // four bands tile [0,1] with no overlaps and no gaps.
    std::sort(bands.begin(), bands.end(),
              [](const WheelRange& a, const WheelRange& b) { return a.lo < b.lo; });
    float running = 0.0f;
    for (auto& band : bands) {
        const float hi = std::clamp(band.hi, running, 1.0f);
        band.lo = running;
        band.hi = hi;
        running = hi;
    }
}

float log_band_weight(float luma, const WheelRange& band, bool uniform) noexcept {
    if (uniform) return 1.0f;
    const float x = clamp01(luma);
    if (x <= band.lo || x >= band.hi) return 0.0f;
    // Cross-fade over a fixed 12.5% of the band at each edge (smoothstep).
    const float frac = band.hi - band.lo;
    const float edge = frac * 0.125f;
    if (edge <= 0.0f) return 1.0f;
    const float lo = smoothstep01(band.lo, std::min(band.lo + edge, band.hi), x);
    const float hi = 1.0f - smoothstep01(std::max(band.hi - edge, band.lo), band.hi, x);
    return std::clamp(lo * hi, 0.0f, 1.0f);
}

float hdr_zone_weight(float luma, const HdrZone& z) noexcept {
    if (z.uniform) return 1.0f;
    const float d = std::abs(clamp01(luma) - z.position);
    if (z.falloff <= 0.0f) return d <= 0.0f ? 1.0f : 0.0f;
    // smoothstep-style falloff: 1 at center easing to 0 at `falloff` distance.
    return 1.0f - smoothstep01(0.0f, std::min(z.falloff, 1.0f), d);
}

LGG lgg_from_cdl(const Cdl& c) noexcept {
    LGG p;
    p.lift_master = 0.0f;
    p.gamma_master = 1.0f;
    p.gain_master = 1.0f;
    // gain=slope, gamma=1/power, lift=0 (dropping CDL offset: no taper => no
    // closed-form Lift equivalent, per spec §5).
    p.gain_r = c.slope_r;
    p.gain_g = c.slope_g;
    p.gain_b = c.slope_b;
    p.gamma_r = c.power_r > 0.0f ? 1.0f / c.power_r : kGammaHi;
    p.gamma_g = c.power_g > 0.0f ? 1.0f / c.power_g : kGammaHi;
    p.gamma_b = c.power_b > 0.0f ? 1.0f / c.power_b : kGammaHi;
    return p;
}

void apply_primaries_wheel(WheelPanelState& s, PrimariesWheel w, float dx, float dy,
                           float master01) noexcept {
    const detail::WheelRanges& meta = detail::kWheelMeta[static_cast<int>(w)];
    const RGBF off = scaled_wheel_offset(dx, dy, meta.scale);
    switch (w) {
        case PrimariesWheel::kLift:
            s.lgg.lift_master = master_to_value(master01, meta.lo_master, meta.hi_master, meta.id_master);
            s.lgg.lift_r = std::clamp(off.r, kLiftLo, kLiftHi);
            s.lgg.lift_g = std::clamp(off.g, kLiftLo, kLiftHi);
            s.lgg.lift_b = std::clamp(off.b, kLiftLo, kLiftHi);
            break;
        case PrimariesWheel::kGamma:
            s.lgg.gamma_master = master_to_value(master01, meta.lo_master, meta.hi_master, meta.id_master);
            s.lgg.gamma_r = std::clamp(1.0f + off.r, kGammaLo, kGammaHi);
            s.lgg.gamma_g = std::clamp(1.0f + off.g, kGammaLo, kGammaHi);
            s.lgg.gamma_b = std::clamp(1.0f + off.b, kGammaLo, kGammaHi);
            break;
        case PrimariesWheel::kGain:
            s.lgg.gain_master = master_to_value(master01, meta.lo_master, meta.hi_master, meta.id_master);
            s.lgg.gain_r = std::clamp(1.0f + off.r, kGainLo, kGainHi);
            s.lgg.gain_g = std::clamp(1.0f + off.g, kGainLo, kGainHi);
            s.lgg.gain_b = std::clamp(1.0f + off.b, kGainLo, kGainHi);
            break;
        case PrimariesWheel::kOffset:
            s.offset.master = master_to_value(master01, meta.lo_master, meta.hi_master, meta.id_master);
            s.offset.r = std::clamp(off.r, kOffsetLo, kOffsetHi);
            s.offset.g = std::clamp(off.g, kOffsetLo, kOffsetHi);
            s.offset.b = std::clamp(off.b, kOffsetLo, kOffsetHi);
            break;
    }
}

void reset_primaries_wheel(WheelPanelState& s, PrimariesWheel w) noexcept {
    switch (w) {
        case PrimariesWheel::kLift:
            s.lgg.lift_master = 0.0f;
            s.lgg.lift_r = s.lgg.lift_g = s.lgg.lift_b = 0.0f;
            break;
        case PrimariesWheel::kGamma:
            s.lgg.gamma_master = 1.0f;
            s.lgg.gamma_r = s.lgg.gamma_g = s.lgg.gamma_b = 1.0f;
            break;
        case PrimariesWheel::kGain:
            s.lgg.gain_master = 1.0f;
            s.lgg.gain_r = s.lgg.gain_g = s.lgg.gain_b = 1.0f;
            break;
        case PrimariesWheel::kOffset:
            s.offset = Offset{};
            break;
    }
}

void reset_panel(WheelPanelState& s) noexcept {
    s = WheelPanelState{};
}

}  // namespace canvas::core::colorsci