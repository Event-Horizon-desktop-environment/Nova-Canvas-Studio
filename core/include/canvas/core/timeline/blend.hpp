#pragma once

// Canonical timeline blend-mode LAW (Qt-free). The per-clip `Clip::blend_mode`
// is `canvas::core::BlendMode` — the timeline's own 5-mode enum, whose order
// (Normal, Add, Multiply, Screen, Overlay) predates and is deliberately kept
// distinct from the color-page `grade_graph::BlendMode` (Normal, Screen,
// Multiply, Overlay, SoftLight, Add, Subtract, Difference). Phase 1 (F6)
// APPENDS the three missing modes (SoftLight, Subtract, Difference) so existing
// project files keep their exact meaning, and moves the byte colour math here so
// the timeline compositor (blit_rgba_transformed) has one tested law.
//
// The byte law is defined THROUGH the grade_graph float law (mapped by mode
// name), so the timeline rendering and the color-page evaluator can never
// disagree on what "Overlay" computes.

#include "canvas/core/timeline/model.hpp"  // canvas::core::BlendMode

#include <cstdint>

namespace canvas::core::blend {

inline constexpr int kBlendModeCount = 8;  // canvas::core::BlendMode 0..7

// Display names in enum order — the Inspector's composite combobox must list
// exactly these, in this order.
[[nodiscard]] const char* blend_mode_name(BlendMode mode) noexcept;

[[nodiscard]] constexpr bool valid_blend_mode(const BlendMode mode) noexcept {
    const int v = static_cast<int>(mode);
    return v >= 0 && v < kBlendModeCount;
}

// Single-channel blend: first the separable blend function B(base, src) in
// normalized [0,1] space, then an opacity dissolve of the blended result over
// the base:
//
//   out = B(base, src) * opacity + base * (1 - opacity)
//
// Identity: Normal + opacity = 1.0 returns `source` exactly (so the caller's
// byte-copy fast path stays valid). Matches the GPU nv12 fade law: with Normal
// + a fade factor f the result is `src*f + base*(1-f)` — the same dissolve, so
// CPU and GPU agree on every faded frame.
[[nodiscard]] uint8_t blend_channel(BlendMode mode, float opacity, uint8_t base,
                                    uint8_t src) noexcept;

}  // namespace canvas::core::blend