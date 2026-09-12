#pragma once

// Custom-curve primary color-grade law (luma + per-channel splines and the
// soft-clip toe/shoulder rails). Qt-free by design, same culture as
// audio_mix.hpp / wheels.hpp, so preview (TimelineDecoder path), export
// (RendererSession), scopes, and the Color page never drift — see
// color-grading-phases.md Phase 5 and color.md Part II.
//
// Model: each channel is an interpolated curve from (0,0) to (1,1). Only the
// INTERIOR control points are stored (the endpoints are implicit), so an empty
// channel vector IS identity regardless of control-point count. If the panel
// lets the user place points at x==0 or x==1 the stored points stay interior
// by construction (the editor clamps them) — the evaluator re-orders and
// clamps defensively, but expects well-formed input.
//
// Interpolation: Fritsch–Carlson monotone cubic (the curve-editor norm — the
// same family Resolve/Photoshop-style tools use), evaluated via the Hermite
// basis over the segment containing x. The spline is monotone wherever the
// control points are monotone and never leaves the box the control points
// span, so "what you drew" is "what renders" (WYSIWYG). Knots are hit exactly
// (the curve passes through every control point). A plain Catmull-Rom was
// originally used — it overshoots between close/stiff knots, pushing values
// outside [0,1] that the final clamp then clips into flat bands and hue
// shifts, which read as a "broken" curve.
//
// Single-channel edits hold luma: editing exactly one of R/G/B (others
// identity) counter-scales the two untouched channels so the edit changes
// color, not exposure — Resolve's unganged-custom-curve default. Editing more
// than one channel, or the luma curve, behaves independently.
//
// Soft clip: toe (low) + shoulder (high) rails. low/high choose the input
// level where the roll-off begins (low == 0 / high == 1 means "full range, no
// fold"); soft in [0,1] is the fold strength — 0 is passthrough, 1 crushes
// everything past the rail flat onto it. The curve is monotone and C1 at the
// rail (slope 1 both sides), so a rail at the edge of the range is a no-op and
// an aggressively folded top still maps 1.0 below 1.0. Applied on the graded
// luma with a ratio scale (see apply_curves), so hue stays parallel.
//
// Every formula operates per-channel on normalized (0..1) RGB unless noted —
// same working-space contract as wheels.hpp.

#include <array>
#include <cmath>
#include <utility>
#include <vector>

#include "canvas/core/colorsci/wheels.hpp"  // RGBF, clamp01

namespace canvas::core::colorsci {

// Rec.601 luma coefficients — the delivery color space of this pipeline's
// decode (matches colorspace.hpp and the scope histogram law).
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

// One interior control point. x,y both in [0,1].
struct CurvePoint {
    float x = 0.0f;
    float y = 0.0f;
};

inline bool operator==(const CurvePoint& a, const CurvePoint& b) noexcept {
    return a.x == b.x && a.y == b.y;
}

// The rails. low_soft/high_soft are the `soft` amount feeding the folded
// smoothstep shoulder law (see eval_soft_clip_high/low); combinations are
// sanity-clamped on write by the panel and defensively here.
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
    // Interior control points per channel; empty == identity channel.
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

// --- spline ----------------------------------------------------------------

// Catmull-Rom curve value at x for the given interior control points; empty
// vector returns x unchanged (identity channel). Regularized so duplicate x
// and ill-sorted points never produce NaN.
[[nodiscard]] float eval_curve(const std::vector<CurvePoint>& points, float x);

// --- soft clip -------------------------------------------------------------

// Shoulder: folds values ABOVE the high rail down toward it. `soft` == 0 is
// passthrough (out == x); `soft` == 1 crushes everything past the rail flat
// onto it. Monotone, C1 at the rail, out(1) < 1 whenever soft > 0. high >= 1
// -> passthrough (nothing to fold).
[[nodiscard]] float eval_soft_clip_high(float x, float high, float soft) noexcept;

// Toe: folds values BELOW the low rail up toward it. `soft` == 0 is
// passthrough; `soft` == 1 crushes everything below the rail flat onto it.
// low <= 0 -> passthrough.
[[nodiscard]] float eval_soft_clip_low(float x, float low, float soft) noexcept;

// --- apply -----------------------------------------------------------------

// Full custom-curve pass: per-channel curves (a single edited R/G/B channel
// holds the pre-curve luma by counter-scaling the untouched ones), then the
// luma curve applied hue-preservingly (Rec.601 luma with a ratio scale so
// parallel colors keep their hue), then the soft-clip toe+shoulder on the
// graded luma (same ratio scale). All steps clamp output to [0,1].
[[nodiscard]] RGBF apply_curves(const RGBF& rgb, const CurveParams& c) noexcept;

}  // namespace canvas::core::colorsci