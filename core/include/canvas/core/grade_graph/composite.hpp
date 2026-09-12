#pragma once

// Qt-free Porter–Duff compositing LAW for grade_graph layer work (Phase 6a).
// The layer-mixer evaluator composes premultiplied interior color with these
// coefficients; the node's key and opacity fold into the source alpha BEFORE
// this module (source_alpha below is already the effective coverage) so the
// law itself never sees a "0..1 gate" — it only sees true alpha.
//
// Why premultiplied interior with a straight boundary: a grade graph stores
// straight RGB (color page convention, json/UI), but compositing two stacked
// images in straight space lets the backdrop bleed through semi-transparent
// edges with wrong magnitudes. The industry answer (Fusion Merge,
// Over/In/HeldOut/Atop/XOr/Disjoint/Mask/Stencil + additive/subtractive knob,
// GafferImage.Merge, SVG "compositing-and-blending" spec) is: fold alpha into
// the color, combine with coverage coefficients, then unpremultiply at the
// boundary. For kOver the top layer's BlendMode family enters as the blend
// function B(Cb, Cs) — the W3C blend-with-over equation
//
//   co = as·(1−ab)·Cs + as·ab·B(Cb,Cs) + (1−as)·ab·Cb
//   ao = as + ab·(1−as)
//
// which degenerates to plain Over when B = Cs (kNormal) and to the old
// straight "replace" behavior when both alphas are 1 — so all legacy opaque
// expectations (blend tests, layer-stack vectors) keep their values.
//
// kDisjoint uses the Over COLOR equation but clamps ao = min(1, as + ab)
// ("correct Alpha combination without going out of range", Fusion). The
// remaining operators composite premultiplied color with fs/fb coefficients
// (no blend family — blend modes only apply to Over-style merging), and the
// Fusion-style additive↔subtractive knob shifts the Over result color between
// the premultiplied difference ("subtractive": backdrop attenuated by 1−as)
// and the premultiplied sum ("additive": backdrop not attenuated, brighter).
// Channels are never clamped (float scene-linear working space, like the phase
// 4/5 grade ops); only alpha is clamped, since coverage is a probability.

#include "canvas/core/grade_graph/graph.hpp"

namespace canvas::core::grade_graph {

// CompositeOp lives in graph.hpp beside BlendMode (the model stores it).

// Source/backdrop coverage coefficients (the classic Porter–Duff table).
// As convention, lowercase f = the fraction of that operand's PREMULTIPLIED
// color/alpha that the operator keeps.
struct CoverageCoefficients {
    float fs = 0.0f;
    float fb = 0.0f;
};

[[nodiscard]] CoverageCoefficients coverage_coefficients(CompositeOp op, float source_alpha,
                                                         float backdrop_alpha);

// Separable blend function B(Cb, Cs) for the Over blend-family. kNormal is the
// identity (returns the source), so Over+kNormal == classic Over.
[[nodiscard]] float blend_channel(BlendMode mode, float backdrop, float source);

// One composed pixel, straight color in, straight color out. `source_alpha` is
// the effective coverage already folded with key*opacity by the caller.
struct CompositeSample {
    float r = 0.0f;
    float g = 0.0f;
    float b = 0.0f;
    float a = 0.0f;
};

[[nodiscard]] CompositeSample composite_sample(float backdrop_r, float backdrop_g, float backdrop_b,
                                               float backdrop_alpha, float source_r, float source_g,
                                               float source_b, float source_alpha,
                                               CompositeOp op, BlendMode blend, float additive);

}  // namespace canvas::core::grade_graph