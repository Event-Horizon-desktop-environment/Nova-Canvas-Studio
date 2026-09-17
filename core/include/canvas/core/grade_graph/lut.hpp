#pragma once

#include "canvas/core/grade_graph/graph.hpp"
#include "canvas/core/media/frame.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace canvas::core::grade_graph {

struct GradeLut3D {
    int size = 0;
    std::vector<float> data;
    std::uint64_t change_seq = 0;

    [[nodiscard]] bool valid() const noexcept {
        return size > 1 && data.size() == static_cast<std::size_t>(size) *
                                                   static_cast<std::size_t>(size) *
                                                   static_cast<std::size_t>(size) * 3u;
    }
};

struct GradeLutDigest {
    std::uint64_t hash = 0;
    float mid[3] = {0.0f, 0.0f, 0.0f};
    float black[3] = {0.0f, 0.0f, 0.0f};
    float white[3] = {0.0f, 0.0f, 0.0f};
    float max_dev = 0.0f;
    std::array<float, 3> skin = {0.0f, 0.0f, 0.0f};
};

[[nodiscard]] GradeLutDigest grade_lut_digest(const GradeLut3D& lut) noexcept;

using GradeLutPtr = std::shared_ptr<const GradeLut3D>;

[[nodiscard]] GradeLutPtr bake_grade_lut(const GradeGraph& g, int size = 33);

[[nodiscard]] VideoFramePtr apply_grade_lut(const VideoFrame& src,
                                            const GradeLut3D& lut);

}
