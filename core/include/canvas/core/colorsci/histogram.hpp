#pragma once

// Stride-sampled column/level accumulation law for the video scopes (parade,
// waveform, histogram). Qt-free, same culture as curves.hpp — the accumulation
// math lives in core so it is exercised by plain unit tests and the Qt widgets
// are left as thin "what to accumulate" + "how to draw it" descriptions.
//
// The geometry fixed here is what the display grid consumes: col*levels cells
// per channel with a level index 0..255 (256 level buckets is visually
// indistinguishable from 1024 for 8-bit input). Sampling is stride-based so a
// full-res frame contributes kHistogramTargetSamples-bounded budget (a
// full-res every-pixel scatter could not keep up at playback).

#include "canvas/core/gpu/colorspace.hpp"
#include "canvas/core/media/frame.hpp"

#include <array>
#include <cstdint>

namespace canvas::core::colorsci {

// Column buckets + 256 level buckets per channel.
inline constexpr int kHistogramCols = 384;
inline constexpr int kHistogramLevels = 256;
inline constexpr int kHistogramCellCount = kHistogramCols * kHistogramLevels;
// Bounded CPU sample budget per frame (stride sampling keeps density shape
// intact and the cost well under the frame budget).
inline constexpr int kHistogramTargetSamples = 262144;

// Channel-interleaved buffers, one plane per channel (0=R, 1=G, 2=B).
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

}  // namespace canvas::core::colorsci