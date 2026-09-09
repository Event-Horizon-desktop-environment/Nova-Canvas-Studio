#pragma once

// Headless wheel-panel controller (color-grading-phases.md Phase 4). Qt-free by
// design (same invariant as the rest of core/), so the whole panel state machine
// and its mapping laws run in plain unit tests; the ColorWheelsPanel in the GUI
// becomes a thin view over this state (color_widgets.cpp).
//
// The four wheel pages (spec §4) are materially different correction models, not
// sub-tabs: Primaries LGG+Offset, Log (Shadow|Midtone|Highlight|Offset with
// zero-overlap tonal bands + low/high range rows), HDR (Dark|Shadow|Light|
// Global zones, each with position/falloff + Exp/Sat), and CDL (slope|offset|
// power + sat with an LGG→CDL readout).
//
// The wheel's job, per spec §1: puck (dx,dy) -> per-channel RGB offsets via
// radius·hue (`puck_xy_to_offset`), composed additively into the per-channel
// terms; the vertical slider beneath each wheel drives ONLY the master term.

#include "canvas/core/colorsci/cdl.hpp"
#include "canvas/core/colorsci/wheels.hpp"

#include <array>
#include <cstddef>

namespace canvas::core::colorsci {

// --- Pagination (spec §4) ---

enum class WheelPage { kPrimaries, kLog, kHdr, kCdl };

constexpr int kWheelPageCount = 4;

// Bound-checked paging. next/prev wrap around the [0,3] range; the explicit
// clamp form exists so external numeric state can't desync the page dots.
[[nodiscard]] WheelPage wheel_page_next(WheelPage p) noexcept;
[[nodiscard]] WheelPage wheel_page_prev(WheelPage p) noexcept;
[[nodiscard]] WheelPage clamp_wheel_page(int index) noexcept;
[[nodiscard]] const char* wheel_page_title(WheelPage p) noexcept;

// --- Wheel puck + master law (spec §1: offset_rgb = radius·hue) ---

// Puck (dx,dy) in [-1,1]^2 -> per-channel offset, scaled to a wheel's max
// reach. dx right = red, dy up = red (hue-ring convention, shared with
// ColorWheelWidget). Zero puck -> zero offset.
[[nodiscard]] inline RGBF scaled_wheel_offset(float dx, float dy, float scale) noexcept {
    const RGBF off = puck_xy_to_offset(dx, dy);
    return RGBF{off.r * scale, off.g * scale, off.b * scale};
}

// Per-wheel maximum offset strength (how far a full-radius puck may reach).
inline constexpr float kWheelLiftScale = 1.0f;
inline constexpr float kWheelGammaScale = 2.0f;
inline constexpr float kWheelGainScale = 1.0f;
inline constexpr float kWheelOffsetScale = 1.0f;

// Master slider (normalized 0..1) <-> term value within [lo,hi], piecewise-
// linear through the identity mid: t01=0.5 -> mid exactly, symmetric towards
// the bounds. Clamped both ways.
[[nodiscard]] float master_to_value(float t01, float lo, float hi, float mid) noexcept;
[[nodiscard]] float value_to_master(float v, float lo, float hi, float mid) noexcept;

// --- Log page (spec §4.1) ---

// One Log wheel's tonal window. Defaults tile [0,1] with zero overlap:
// Shadow [0,1/3], Midtone [1/3,2/3], Highlight [2/3,1], Offset uniform. The
// zero-overlap law below keeps any user-adjusted set a tile: sorted by low,
// then each band's low is pinned to the previous band's high and the high is
// clamped into [0,1], so bands never overlap and never leave gaps.
struct WheelRange {
    float lo = 0.0f;
    float hi = 1.0f;
};

struct LogWheelState {
    LGG lgg;                 // Shadow/Midtone/Highlight: LGG-shaped params
    WheelRange band{0.0f, 1.0f};
    bool uniform = false;    // the Offset wheel applies everywhere (weight 1)
};

// Zero-overlap partition for exactly the four Log bands (spec: "Log wheels
// have zero overlap" vs Primaries' natural overlap).
void enforce_zero_overlap(std::array<WheelRange, 4>& bands) noexcept;

// Band weight with cross-fading at the range edges: smoothstep up at lo, hold
// 1 through the middle, smoothstep down at hi; 0 outside. Uniform bands return
// 1 everywhere.
[[nodiscard]] float log_band_weight(float luma, const WheelRange& band, bool uniform) noexcept;

// --- HDR page (spec §4.2) ---

// A generalized N-zone system: each zone is centered at `position` along the
// tonal range with `falloff` width, its wheel yields per-channel offsets, and
// Exp/Sat shift exposure/saturation within the zone. Global is the uniform
// zone (weight 1 everywhere). Extensible beyond the stock four.
struct HdrZone {
    float position = 0.5f;
    float falloff = 0.25f;
    float exp = 0.0f;    // exposure shift in EV-ish stops, 0 = identity
    float sat = 0.0f;    // saturation shift, 0 = identity (kZoneSatLo..Hi)
    RGBF offset;         // wheel puck -> per-channel offset (already scaled)
    bool uniform = false;
};

inline constexpr int kHdrStockZones = 4;   // Dark, Shadow, Light, Global
inline constexpr int kHdrMaxZones = 12;    // headroom for user-created zones

// Zone weighting over luma: smoothstep-style falloff on |luma - position| vs
// falloff, 1 at the zone center easing to 0 at `falloff` distance. The Global
// zone (uniform) returns exactly 1.0 everywhere per spec §4.2.4.
[[nodiscard]] float hdr_zone_weight(float luma, const HdrZone& z) noexcept;

// --- CDL page (spec §5, interchange) ---

// LGG -> CDL lives in cdl.hpp (cdl_from_lgg), exact only when the LGG lift is
// 0. The inverse (CDL -> LGG) drops CDL offset (Lift's taper has no closed-form
// CDL equivalent) and yields gain=slope, gamma=1/power with lift=0:
// round-trip is exact exactly for offset-free CDL rows.
[[nodiscard]] LGG lgg_from_cdl(const Cdl& c) noexcept;

// --- Panel state (per selected clip) ---

// Tone-row parameter slots (spec §4, item 4): the ONE shared row BELOW the
// wheels. Enum order is the panel's display order — Temp, Tint, Hue, Contrast,
// Pivot, Mid/Detail, Blk/Offset — matching the mockup's shared row exactly.
// The widget maps each ToneField to one slot and every change lands on the
// same WheelPanelState.
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

// Temp/Tint/Mid-Detail share the ±100 scale; the rest use wheels.hpp ranges
// directly.
inline constexpr float kTempLo = -100.0f;
inline constexpr float kTempHi = 100.0f;
inline constexpr float kTintLo = -100.0f;
inline constexpr float kTintHi = 100.0f;
inline constexpr float kMidDetailLo = -100.0f;
inline constexpr float kMidDetailHi = 100.0f;

// What the Color page's wheel panel edits. All identity defaults: a fresh state
// is a no-op. `page` is the active wheel page; the Primaries/Log/HDR/CDL
// param blocks keep their values when switching pages (state, not tabs).
struct WheelPanelState {
    WheelPage page = WheelPage::kPrimaries;

    // Primaries page (§1): the LGG trio + the separate Offset wheel.
    LGG lgg;
    Offset offset;

    // Tone rows (spec §4 item 4): ONE shared row BELOW the wheels — Temp, Tint,
    // Hue, Contrast, Pivot, Mid/Detail, Blk/Offset (mockup order). black_offset
    // drives the Offset wheel's master term (same value range), so the field and
    // the Offset cassette knob stay in lock-step. The legacy row-2 params below
    // (color_boost … lum_mix) are retained in state for data compatibility but
    // no longer surfaced in the panel UI.
    float temp = 0.0f;                    // ±100 (warm red / cool blue)
    float tint = 0.0f;                    // ±100 (green / magenta)
    float hue_deg = 0.0f;                 // kHueDegLo..Hi
    float contrast = 1.0f;                // kContrastLo..Hi
    float pivot = kDefaultPivot;          // kPivotLo..Hi
    float mid_detail = 0.0f;              // ±100 (spatial unsharp; Phase 7)
    float black_offset = 0.0f;            // kOffsetLo..Hi, mirrors offset.master
    float color_boost = 0.0f;             // retained, UI-hidden
    float shadows_sat = 0.0f;             // retained, UI-hidden
    float highlights_sat = 0.0f;          // retained, UI-hidden
    float saturation = 1.0f;              // retained, UI-hidden
    float lum_mix = 1.0f;                 // retained, UI-hidden

    // Log page (§4.1): four LGG-shaped terms + their zero-overlap bands.
    std::array<LogWheelState, 4> log{};

    // HDR page (§4.2): stock zones, extendable to kHdrMaxZones.
    std::array<HdrZone, kHdrStockZones> hdr{};

    // CDL page (§5): the interchangeable row + readout of the Primaries LGG.
    Cdl cdl;
};

// Primaries wheel application: a wheel drag writes per-channel offsets into the
// LGG/Offset fields it owns (Lift/Gamma/Gain/Offset) and the slider maps
// master01 -> the term's master value. p.lift_master (etc.) come from the
// slider; the per-channel terms come from the puck. Clamped to the shared
// wheels.hpp ranges so edit-ops and the evaluator never see illegal values.
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
}  // namespace detail

// Applies a puck + master slider to one Primaries wheel of the state.
// `master01` is the slider position (0..1). A multi-wheel panel holds one
// (puck, master01) pair per wheel and calls this on every drag/move.
void apply_primaries_wheel(WheelPanelState& s, PrimariesWheel w, float dx, float dy,
                           float master01) noexcept;

// Reset a single term (primaries page) or the whole panel to identity.
void reset_primaries_wheel(WheelPanelState& s, PrimariesWheel w) noexcept;
void reset_panel(WheelPanelState& s) noexcept;

}  // namespace canvas::core::colorsci