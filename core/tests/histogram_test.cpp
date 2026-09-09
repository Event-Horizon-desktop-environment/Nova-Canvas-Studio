// Phase 5 tests for the scope histogram accumulation law (colorsci/histogram,
// extracted headless from the Qt ColumnHistogram). Verifies the stride-sampled
// RGBA/NV12 -> column/level density planes: single-pixel cell mapping, sample
// budget on large frames, luma = Rec.601 for RGBA and the Y plane for NV12.
// Links only canvas_core.

#include "canvas/core/colorsci/histogram.hpp"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <numeric>

namespace cs = canvas::core::colorsci;

namespace {

int failures = 0;

void check(bool cond, const char* what) {
    if (!cond) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++failures;
    } else {
        std::printf("ok:   %s\n", what);
    }
}

canvas::core::VideoFrame make_rgba_frame(int w, int h, std::uint8_t r, std::uint8_t g,
                                         std::uint8_t b) {
    canvas::core::VideoFrame f;
    f.width = w;
    f.height = h;
    f.stride = std::size_t(w) * 4u;
    f.rgba.assign(static_cast<std::size_t>(w) * h * 4u, 0);
    for (std::size_t i = 0; i < f.rgba.size(); i += 4) {
        f.rgba[i] = r;
        f.rgba[i + 1] = g;
        f.rgba[i + 2] = b;
        f.rgba[i + 3] = 255;
    }
    return f;
}

std::uint64_t sum(const std::array<std::uint32_t, cs::kHistogramCellCount>& a) {
    return std::accumulate(a.begin(), a.end(), std::uint64_t{0});
}

std::uint64_t sum_channels(const std::array<std::uint32_t,
                                            cs::kHistogramChannelCount * cs::kHistogramCellCount>& a) {
    return std::accumulate(a.begin(), a.end(), std::uint64_t{0});
}

void test_clear() {
    cs::ColumnHistogram h;
    const auto f = make_rgba_frame(64, 64, 200, 100, 50);
    h.accumulate(f);
    check(sum_channels(h.channels()) > 0, "accumulate fills the buffers");
    h.clear();
    check(sum(h.luma()) == 0 && sum_channels(h.channels()) == 0, "clear empties the buffers");
}

void test_single_pixel() {
    cs::ColumnHistogram h;
    const auto f = make_rgba_frame(1, 1, 255, 128, 64);
    h.accumulate(f);

    // Column 0 (1 pixel wide -> xoff == 0), levels == channel values.
    check(h.channels()[0 * cs::kHistogramCellCount + 255] == 1, "red lands at level 255");
    check(h.channels()[1 * cs::kHistogramCellCount + 128] == 1, "green lands at level 128");
    check(h.channels()[2 * cs::kHistogramCellCount + 64] == 1, "blue lands at level 64");
    // Rec.601 luma: 0.299*255 + 0.587*128 + 0.114*64 round-trip via 8.8 fixed.
    const std::uint32_t luma = (299u * 255u + 587u * 128u + 114u * 64u) / 1000u;
    check(h.luma()[luma] == 1, "RGBA luma uses Rec.601 weights");
}

void test_stride_budget() {
    // 2048x512 with 2x2 stride sampling: 1024x256 == 262144 == the budget, and
    // every sampled pixel is counted once in each plane (the budget first
    // kicks in when w*h >= 4*kHistogramTargetSamples; smaller frames sample
    // every pixel).
    cs::ColumnHistogram h;
    const auto f = make_rgba_frame(2048, 512, 31, 127, 219);
    h.accumulate(f);
    check(sum_channels(h.channels()) == cs::kHistogramTargetSamples * 3,
          "sample budget is honored on a full-res frame");
    check(sum(h.luma()) == cs::kHistogramTargetSamples, "luma plane samples once per pixel");
}

void test_nv12_luma_is_y() {
    cs::ColumnHistogram h;
    canvas::core::Nv12Frame f;
    f.width = 4;
    f.height = 4;
    f.y_pitch = 4;
    f.uv_pitch = 4;
    const std::uint8_t yval = 200;
    const std::uint8_t cval = 128;  // neutral chroma -> Y only (yuv_to_rgb: r=g=b=Y)
    f.y.assign(16, yval);
    f.uv.assign(8, cval);
    h.accumulate(f);

    // 16 samples; each row maps to columns 0,96,192,288 (col*384/4).
    check(sum(h.luma()) == 16, "NV12 scrubs each luma sample");
    for (const int col : {0, 96, 192, 288}) {
        const std::size_t off = std::size_t(col) * cs::kHistogramLevels + yval;
        check(h.luma()[off] == 4, "NV12 luma lands on the Y plane value per column");
    }
    check(sum_channels(h.channels()) == 16 * 3, "NV12 chroma decoded to RGB per sample");
}

void test_multiple_values() {
    // Two-tone frame: RGB 0 and RGB 255 in one row each. Both levels appear in
    // every channel's level buckets (column 0).
    canvas::core::VideoFrame f;
    f.width = 1;
    f.height = 2;
    f.stride = 4;
    f.rgba = {0, 0, 0, 255, 255, 255, 255, 255};
    cs::ColumnHistogram h;
    h.accumulate(f);
    check(h.channels()[0 * cs::kHistogramCellCount + 0] == 1, "black present in red plane");
    check(h.channels()[0 * cs::kHistogramCellCount + 255] == 1, "white present in red plane");
    check(sum(h.luma()) == 2, "two rows contribute two luma samples");
    check(h.luma()[0] == 1, "black luma at level 0");
    check(h.luma()[255] == 1, "white luma at level 255");
}

}  // namespace

int main() {
    test_clear();
    test_single_pixel();
    test_stride_budget();
    test_nv12_luma_is_y();
    test_multiple_values();

    if (failures == 0) {
        std::printf("all checks passed\n");
        return EXIT_SUCCESS;
    }
    std::fprintf(stderr, "%d check(s) FAILED\n", failures);
    return EXIT_FAILURE;
}