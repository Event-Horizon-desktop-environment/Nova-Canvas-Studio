// ColumnHistogram accumulation law — stride-sampled RGBA/NV12 -> column/level
// density planes. Qt-free; the Qt scopes reduce these buffers at render time.
// Header `histogram.hpp` documents the geometry; this file is the math.

#include "canvas/core/colorsci/histogram.hpp"

#include <algorithm>
#include <cmath>

namespace canvas::core::colorsci {

void ColumnHistogram::clear() {
    hist_.fill(0);
    hist_luma_.fill(0);
}

void ColumnHistogram::accumulate(const canvas::core::VideoFrame& rgba) {
    const int w = rgba.width;
    const int h = rgba.height;
    if (w <= 0 || h <= 0) return;
    const std::size_t stride = rgba.stride != 0 ? rgba.stride : std::size_t(w) * 4;

    // Stride-sample so the total pixel count lands at/under kHistogramTargetSamples.
    const double need =
        std::sqrt(double(w) * double(h) / double(kHistogramTargetSamples));
    const int sy = std::max(1, static_cast<int>(need));
    const int sx = std::max(1, static_cast<int>(need));

    const std::uint8_t* base = rgba.rgba.data();
    for (int row = 0; row < h; row += sy) {
        const std::uint8_t* p = base + std::size_t(row) * stride;
        for (int col = 0; col < w; col += sx) {
            const std::size_t o = std::size_t(col) * 4;
            const std::uint32_t lvl_r = p[o + 0];
            const std::uint32_t lvl_g = p[o + 1];
            const std::uint32_t lvl_b = p[o + 2];
            const std::size_t xoff = std::size_t((col * kHistogramCols) / w) * kHistogramLevels;
            hist_[0 * kHistogramCellCount + xoff + lvl_r]++;
            hist_[1 * kHistogramCellCount + xoff + lvl_g]++;
            hist_[2 * kHistogramCellCount + xoff + lvl_b]++;
            // Rec.601 luma matching the BT.601 decode pipeline (key the matrix
            // off your pipeline's color space).
            hist_luma_[xoff + (299u * lvl_r + 587u * lvl_g + 114u * lvl_b) / 1000u]++;
        }
    }
}

void ColumnHistogram::accumulate(const canvas::core::Nv12Frame& nv12) {
    const int w = nv12.width;
    const int h = nv12.height;
    if (w <= 0 || h <= 0) return;

    const double need =
        std::sqrt(double(w) * double(h) / double(kHistogramTargetSamples));
    const int sy = std::max(1, static_cast<int>(need));
    const int sx = std::max(1, static_cast<int>(need));

    const std::uint8_t* y = nv12.y.data();
    const std::uint8_t* uv = nv12.uv.data();
    for (int row = 0; row < h; row += sy) {
        const std::uint8_t* yrow = y + std::size_t(row) * nv12.y_pitch;
        for (int col = 0; col < w; col += sx) {
            const std::size_t uvo = std::size_t((row / 2) * nv12.uv_pitch) +
                                    std::size_t((col / 2) * 2);
            const auto rgb = canvas::core::gpu::yuv_to_rgb(yrow[col], uv[uvo + 0], uv[uvo + 1]);
            const std::size_t xoff =
                std::size_t((col * kHistogramCols) / w) * kHistogramLevels;
            hist_[0 * kHistogramCellCount + xoff + rgb.r]++;
            hist_[1 * kHistogramCellCount + xoff + rgb.g]++;
            hist_[2 * kHistogramCellCount + xoff + rgb.b]++;
            // NV12's Y plane IS the luma trace — no matrix needed.
            hist_luma_[xoff + yrow[col]]++;
        }
    }
}

}  // namespace canvas::core::colorsci