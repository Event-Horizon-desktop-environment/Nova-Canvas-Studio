#pragma once

#include "canvas/core/gpu/colorspace.hpp"
#include "canvas/core/media/frame.hpp"

#include <array>
#include <cstdint>

namespace canvas::core::colorsci {

inline constexpr int kHistogramCols = 384;
inline constexpr int kHistogramLevels = 256;
inline constexpr int kHistogramCellCount = kHistogramCols * kHistogramLevels;
inline constexpr int kHistogramTargetSamples = 262144;

inline constexpr int kHistogramChannelCount = 3;

class ColumnHistogram {
public:
    void clear();
    void accumulate(const canvas::core::VideoFrame& rgba);
    void accumulate(const canvas::core::Nv12Frame& nv12);

    [[nodiscard]] const std::array<std::uint32_t, kHistogramChannelCount * kHistogramCellCount>&
    channels() const {
        return hist_;
    }
    [[nodiscard]] constexpr int cols() const noexcept { return kHistogramCols; }
    [[nodiscard]] constexpr int levels() const noexcept { return kHistogramLevels; }
    [[nodiscard]] const std::array<std::uint32_t, kHistogramCellCount>& luma() const {
        return hist_luma_;
    }

private:
    std::array<std::uint32_t, kHistogramChannelCount * kHistogramCellCount> hist_{};
    std::array<std::uint32_t, kHistogramCellCount> hist_luma_{};
};

}
