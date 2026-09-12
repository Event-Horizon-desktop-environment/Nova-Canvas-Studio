#pragma once

// CDL (ASC Color Decision List) math law. Header-only / Qt-free, same culture
// as audio_mix.hpp. Secondary correction model used by the CDL wheel page and
// the LGG<->CDL readout. See color-wheels-grading-math-spec.md (CDL page).
//
//   out = ( slope * in + offset ) ^ power     (per channel, base clamped >= 0)
//   then Rec.709 luma-weighted saturation applied to the pow'd result.
//
// Closed-form LGG<->CDL conversion exists only for lift == 0 (see
// cdl_from_lgg); a non-zero lift has no closed-form CDL equivalent, so the
// conversion reports exact = false rather than approximating silently.

#include <algorithm>
#include <cmath>

#include "canvas/core/colorsci/wheels.hpp"

namespace canvas::core::colorsci {

struct Cdl {
    float slope_r = 1.0f, slope_g = 1.0f, slope_b = 1.0f;
    float offset_r = 0.0f, offset_g = 0.0f, offset_b = 0.0f;
    float power_r = 1.0f, power_g = 1.0f, power_b = 1.0f;
    float sat = 1.0f;
};

[[nodiscard]] inline float cdl_channel(float in, float slope, float offset, float power) noexcept {
    const float base = slope * in + offset;
    if (base <= 0.0f) return 0.0f;  // clamp lower bound before the fractional power
    return std::pow(base, std::clamp(power, 0.1f, 10.0f));
}

[[nodiscard]] inline RGBF apply_cdl(const RGBF& in, const Cdl& c) noexcept {
    RGBF out;
    out.r = cdl_channel(in.r, c.slope_r, c.offset_r, c.power_r);
    out.g = cdl_channel(in.g, c.slope_g, c.offset_g, c.power_g);
    out.b = cdl_channel(in.b, c.slope_b, c.offset_b, c.power_b);
    return apply_saturation(out, c.sat);
}

struct CdlConversion {
    Cdl cdl;
    bool exact;  // false when the source LGG had a non-zero lift
};

// LGG -> CDL. With lift == 0 the algebra collapses exactly:
//   LGG: (g * x)^(1/gamma)  ==  CDL: (s*x + o)^p  with s=g, o=0, p=1/gamma.
// Any non-zero lift makes the forms incommensurate; we still emit the best
// scale/shape match (same gain/gamma, lift dropped) but flag exact = false.
[[nodiscard]] inline CdlConversion cdl_from_lgg(const LGG& p) noexcept {
    CdlConversion conv;
    conv.cdl.slope_r = p.gain_master * p.gain_r;
    conv.cdl.slope_g = p.gain_master * p.gain_g;
    conv.cdl.slope_b = p.gain_master * p.gain_b;
    // CDL power is the raw exponent; LGG uses (1/gamma).
    conv.cdl.power_r = 1.0f / (p.gamma_master * p.gamma_r);
    conv.cdl.power_g = 1.0f / (p.gamma_master * p.gamma_g);
    conv.cdl.power_b = 1.0f / (p.gamma_master * p.gamma_b);
    conv.cdl.offset_r = conv.cdl.offset_g = conv.cdl.offset_b = 0.0f;
    conv.cdl.sat = 1.0f;
    const float lift_any = p.lift_master + p.lift_r + p.lift_g + p.lift_b;
    const bool gamma_ok =
        (p.gamma_master * p.gamma_r) > 0.0f && (p.gamma_master * p.gamma_g) > 0.0f &&
        (p.gamma_master * p.gamma_b) > 0.0f;
    conv.exact = gamma_ok && std::abs(lift_any) < 1e-6f;
    return conv;
}

}  // namespace canvas::core::colorsci