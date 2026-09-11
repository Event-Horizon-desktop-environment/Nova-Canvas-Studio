#include "canvas/core/grade_graph/composite.hpp"

#include <algorithm>
#include <cmath>

namespace canvas::core::grade_graph {

CoverageCoefficients coverage_coefficients(CompositeOp op, float source_alpha,
                                           float backdrop_alpha) {
    const float as = std::clamp(source_alpha, 0.0f, 1.0f);
    const float ab = std::clamp(backdrop_alpha, 0.0f, 1.0f);
    switch (op) {
        case CompositeOp::kIn:
            return {ab, 0.0f};
        case CompositeOp::kOut:
            return {1.0f - ab, 0.0f};
        case CompositeOp::kAtop:
            return {ab, 1.0f - as};
        case CompositeOp::kXor:
            return {1.0f - ab, 1.0f - as};
        case CompositeOp::kMask:
            return {0.0f, as};
        case CompositeOp::kStencil:
            return {0.0f, 1.0f - as};
        case CompositeOp::kOver:
        case CompositeOp::kDisjoint:
        default:
            return {1.0f, 1.0f - as};
    }
}

float blend_channel(BlendMode mode, float backdrop, float source) {
    switch (mode) {
        case BlendMode::kScreen:
            return 1.0f - (1.0f - backdrop) * (1.0f - source);
        case BlendMode::kMultiply:
            return backdrop * source;
        case BlendMode::kOverlay:
            return backdrop <= 0.5f ? 2.0f * backdrop * source
                                    : 1.0f - 2.0f * (1.0f - backdrop) * (1.0f - source);
        case BlendMode::kSoftLight:
            if (source <= 0.5f)
                return backdrop - (1.0f - 2.0f * source) * backdrop * (1.0f - backdrop);
            if (backdrop <= 0.25f)
                return backdrop +
                       (2.0f * source - 1.0f) * (((16.0f * backdrop - 12.0f) * backdrop + 4.0f) *
                                                 backdrop);
            return backdrop + (2.0f * source - 1.0f) * (std::sqrt(backdrop) - backdrop);
        case BlendMode::kAdd:
            return backdrop + source;
        case BlendMode::kSubtract:
            return backdrop - source;
        case BlendMode::kDifference:
            return std::fabs(backdrop - source);
        case BlendMode::kNormal:
        default:
            return source;
    }
}

namespace {

// Straight color delivered by a premultiplied combination, guarding zero alpha.
float unpremultiply(float premul, float alpha) {
    return alpha > 0.0f ? premul / alpha : 0.0f;
}

}  // namespace

CompositeSample composite_sample(float backdrop_r, float backdrop_g, float backdrop_b,
                                 float backdrop_alpha, float source_r, float source_g,
                                 float source_b, float source_alpha, CompositeOp op,
                                 BlendMode blend, float additive) {
    const float as = std::clamp(source_alpha, 0.0f, 1.0f);
    const float ab = std::clamp(backdrop_alpha, 0.0f, 1.0f);

    CompositeSample s;
    const float t = std::clamp(additive, 0.0f, 1.0f);
    const bool over_like = op == CompositeOp::kOver || op == CompositeOp::kDisjoint;
    if (over_like) {
        // W3C blend-with-over color (premultiplied result), then un-premultiply.
        const auto over_color = [&](float Cb, float Cs) {
            const float B = blend_channel(blend, Cb, Cs);
            return as * (1.0f - ab) * Cs + as * ab * B + (1.0f - as) * ab * Cb;
        };
        s.a = op == CompositeOp::kDisjoint ? std::min(1.0f, as + ab) : as + ab * (1.0f - as);
        s.r = unpremultiply(over_color(backdrop_r, source_r), s.a);
        s.g = unpremultiply(over_color(backdrop_g, source_g), s.a);
        s.b = unpremultiply(over_color(backdrop_b, source_b), s.a);
        if (t > 0.0f) {
            // Fusion additive/subtractive knob: mix toward the premultiplied
            // SUM (backdrop not attenuated by 1−as), which is brighter.
            const float sum_r = unpremultiply(source_r * as + backdrop_r * ab, s.a);
            const float sum_g = unpremultiply(source_g * as + backdrop_g * ab, s.a);
            const float sum_b = unpremultiply(source_b * as + backdrop_b * ab, s.a);
            s.r = s.r + (sum_r - s.r) * t;
            s.g = s.g + (sum_g - s.g) * t;
            s.b = s.b + (sum_b - s.b) * t;
        }
        return s;
    }

    const CoverageCoefficients c = coverage_coefficients(op, as, ab);
    s.a = std::clamp(c.fs * as + c.fb * ab, 0.0f, 1.0f);
    s.r = unpremultiply(c.fs * source_r * as + c.fb * backdrop_r * ab, s.a);
    s.g = unpremultiply(c.fs * source_g * as + c.fb * backdrop_g * ab, s.a);
    s.b = unpremultiply(c.fs * source_b * as + c.fb * backdrop_b * ab, s.a);
    return s;
}

}  // namespace canvas::core::grade_graph