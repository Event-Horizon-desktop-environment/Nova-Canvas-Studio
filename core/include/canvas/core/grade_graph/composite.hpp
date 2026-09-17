#pragma once

#include "canvas/core/grade_graph/graph.hpp"

namespace canvas::core::grade_graph {

struct CoverageCoefficients {
    float fs = 0.0f;
    float fb = 0.0f;
};

[[nodiscard]] CoverageCoefficients coverage_coefficients(CompositeOp op, float source_alpha,
                                                         float backdrop_alpha);

[[nodiscard]] float blend_channel(BlendMode mode, float backdrop, float source);

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

}
