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

    check(h.channels()[0 * cs::kHistogramCellCount + 255] == 1, "red lands at level 255");
    check(h.channels()[1 * cs::kHistogramCellCount + 128] == 1, "green lands at level 128");
    check(h.channels()[2 * cs::kHistogramCellCount + 64] == 1, "blue lands at level 64");
    const std::uint32_t luma = (299u * 255u + 587u * 128u + 114u * 64u) / 1000u;
    check(h.luma()[luma] == 1, "RGBA luma uses Rec.601 weights");
}

void test_stride_budget() {
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
    const std::uint8_t cval = 128;
    f.y.assign(16, yval);
    f.uv.assign(8, cval);
    h.accumulate(f);

    check(sum(h.luma()) == 16, "NV12 scrubs each luma sample");
    for (const int col : {0, 96, 192, 288}) {
        const std::size_t off = std::size_t(col) * cs::kHistogramLevels + yval;
        check(h.luma()[off] == 4, "NV12 luma lands on the Y plane value per column");
    }
    check(sum_channels(h.channels()) == 16 * 3, "NV12 chroma decoded to RGB per sample");
}

void test_multiple_values() {
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

}

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
