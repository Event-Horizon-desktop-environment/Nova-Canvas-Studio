#pragma once

#include "canvas/core/grade_graph/graph.hpp"

#include <memory>
#include <string>
#include <vector>

namespace canvas::core::grade_graph {

struct FrameF {
    int w = 0;
    int h = 0;
    std::vector<float> rgba;

    static FrameF filled(int w, int h, float r, float g, float b, float a = 1.0f);
    [[nodiscard]] float* at(int x, int y);
    [[nodiscard]] const float* at(int x, int y) const;
};

struct GrayF {
    int w = 0;
    int h = 0;
    std::vector<float> v;

    static GrayF filled(int w, int h, float value);
};

struct EvalResult {
    std::shared_ptr<const FrameF> frame;
    std::string error;
};

[[nodiscard]] EvalResult evaluate_graph(const GradeGraph& g, const FrameF& source);

void blend_into(const float* acc, const float* layer, float* out, std::size_t n,
                BlendMode blend);

}
