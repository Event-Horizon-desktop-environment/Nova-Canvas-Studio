#include "canvas/core/grade_graph/lut.hpp"

#include "canvas/core/grade_graph/eval.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>

namespace canvas::core::grade_graph {

namespace {

constexpr float kByteToUnit = 1.0f / 255.0f;

std::uint8_t clamp_unit(float v) {
    v = std::max(0.0f, std::min(1.0f, v));
    return static_cast<std::uint8_t>(std::lround(v * 255.0f));
}

// Trilinear sample of a baked LUT at a unit-scale input (r, g, b each in
// [0,1]), mirroring the texel-center/spatial convention of apply_grade_lut so
// a digest probe reports the exact same value a graded pixel would draw.
[[nodiscard]] std::array<float, 3> sample_lut_pixel(const GradeLut3D& lut,
                                                    std::array<float, 3> in) noexcept {
    std::array<float, 3> out{0.0f, 0.0f, 0.0f};
    if (!lut.valid()) return out;
    const int n = lut.size;
    const int last = n - 1;

    const float ur = std::clamp(in[0], 0.0f, 1.0f) * static_cast<float>(last);
    const float vg = std::clamp(in[1], 0.0f, 1.0f) * static_cast<float>(last);
    const float wb = std::clamp(in[2], 0.0f, 1.0f) * static_cast<float>(last);

    int r0 = static_cast<int>(ur);
    int g0 = static_cast<int>(vg);
    int b0 = static_cast<int>(wb);
    r0 = std::clamp(r0, 0, last);
    g0 = std::clamp(g0, 0, last);
    b0 = std::clamp(b0, 0, last);
    const int r1 = std::min(r0 + 1, last);
    const int g1 = std::min(g0 + 1, last);
    const int b1 = std::min(b0 + 1, last);
    const float fr = ur - static_cast<float>(r0);
    const float fg = vg - static_cast<float>(g0);
    const float fb = wb - static_cast<float>(b0);

    // Corner indices derive from the (r?,g?,b?) tuples directly. When an input
    // sits exactly on the grid edge r1/r0==last the naive i100=i000+n*n would
    // read one row past N-1 (a latent OOB the zero edge-weight masked); clamping
    // the HIGH neighbor to the floor keeps every read in-bounds while keeping
    // the interpolation bit-identical. This index law is mirrored 1:1 by the
    // CUDA nv12GradeResize kernel — never change one without the other.
    const std::vector<float>& d = lut.data;
    const std::size_t nsz = static_cast<std::size_t>(n);
    const std::size_t i000 = (static_cast<std::size_t>(r0) * nsz + static_cast<std::size_t>(g0)) * nsz +
                             static_cast<std::size_t>(b0);
    const std::size_t i100 = (static_cast<std::size_t>(r1) * nsz + static_cast<std::size_t>(g0)) * nsz +
                             static_cast<std::size_t>(b0);
    const std::size_t i001 = (static_cast<std::size_t>(r0) * nsz + static_cast<std::size_t>(g0)) * nsz +
                             static_cast<std::size_t>(b1);
    const std::size_t i101 = (static_cast<std::size_t>(r1) * nsz + static_cast<std::size_t>(g0)) * nsz +
                             static_cast<std::size_t>(b1);
    const std::size_t i010 = (static_cast<std::size_t>(r0) * nsz + static_cast<std::size_t>(g1)) * nsz +
                             static_cast<std::size_t>(b0);
    const std::size_t i110 = (static_cast<std::size_t>(r1) * nsz + static_cast<std::size_t>(g1)) * nsz +
                             static_cast<std::size_t>(b0);
    const std::size_t i011 = (static_cast<std::size_t>(r0) * nsz + static_cast<std::size_t>(g1)) * nsz +
                             static_cast<std::size_t>(b1);
    const std::size_t i111 = (static_cast<std::size_t>(r1) * nsz + static_cast<std::size_t>(g1)) * nsz +
                             static_cast<std::size_t>(b1);

    for (int c = 0; c < 3; ++c) {
        const std::size_t s000 = i000 * 3u + static_cast<std::size_t>(c);
        const std::size_t s100 = i100 * 3u + static_cast<std::size_t>(c);
        const std::size_t s001 = i001 * 3u + static_cast<std::size_t>(c);
        const std::size_t s101 = i101 * 3u + static_cast<std::size_t>(c);
        const std::size_t s010 = i010 * 3u + static_cast<std::size_t>(c);
        const std::size_t s110 = i110 * 3u + static_cast<std::size_t>(c);
        const std::size_t s011 = i011 * 3u + static_cast<std::size_t>(c);
        const std::size_t s111 = i111 * 3u + static_cast<std::size_t>(c);

        const float c00 = d[s000] + (d[s001] - d[s000]) * fb;
        const float c10 = d[s100] + (d[s101] - d[s100]) * fb;
        const float c01 = d[s010] + (d[s011] - d[s010]) * fb;
        const float c11 = d[s110] + (d[s111] - d[s110]) * fb;
        const float c0 = c00 + (c01 - c00) * fg;
        const float c1 = c10 + (c11 - c10) * fg;
        out[static_cast<std::size_t>(c)] = c0 + (c1 - c0) * fr;
    }
    return out;
}

}  // namespace

GradeLutPtr bake_grade_lut(const GradeGraph& g, int size) {
    if (size < 2 || g.terminal() < 0) return nullptr;

    // The graph is pointwise per-pixel (color-key mattes, no spatial operators
    // yet), so every grid point evaluates independently. Bake the whole grid as
    // ONE image: a w=size x h=size*size frame whose pixel p = y*size + x carries
    // the grid coordinate (ri=x, gi=y/size, bi=y%size). One evaluate_graph call
    // replaces size^3 separate 1x1 evaluations, keeping the bake sub-millisecond
    // (the previous all-at-once-vs-loop difference matters: 33^3 = 35937).
    FrameF grid;
    grid.w = size;
    grid.h = size * size;
    grid.rgba.assign(static_cast<std::size_t>(grid.w) * static_cast<std::size_t>(grid.h) * 4u,
                     0.0f);
    const float inv = 1.0f / static_cast<float>(size - 1);
    for (int y = 0; y < grid.h; ++y) {
        const int gi = y / size;
        const int bi = y % size;
        for (int x = 0; x < grid.w; ++x) {
            const int ri = x;
            float* px = grid.at(x, y);
            px[0] = static_cast<float>(ri) * inv;
            px[1] = static_cast<float>(gi) * inv;
            px[2] = static_cast<float>(bi) * inv;
            px[3] = 1.0f;
        }
    }

    const EvalResult res = evaluate_graph(g, grid);
    if (!res.frame || !res.error.empty()) return nullptr;  // no terminal / inactive

    auto lut = std::make_shared<GradeLut3D>();
    lut->size = size;
    // Carries the graph's change-token so the decode-side [grade] LUT-baked
    // line can correlate to the GUI commit (and the viewer upload) by seq.
    lut->change_seq = g.change_seq;
    lut->data.assign(static_cast<std::size_t>(size) * static_cast<std::size_t>(size) *
                         static_cast<std::size_t>(size) * 3u,
                     0.0f);
    for (int p = 0; p < grid.w * grid.h; ++p) {
        const int y = p / grid.w;
        const int x = p % grid.w;
        const int gi = y / size;
        const int bi = y % size;
        const std::size_t idx = (static_cast<std::size_t>(x) * static_cast<std::size_t>(size) +
                                 static_cast<std::size_t>(gi)) *
                                    static_cast<std::size_t>(size) +
                                static_cast<std::size_t>(bi);
        const float* src = res.frame->at(x, y);
        lut->data[idx * 3u + 0u] = src[0];
        lut->data[idx * 3u + 1u] = src[1];
        lut->data[idx * 3u + 2u] = src[2];
    }
    return lut;
}

GradeLutDigest grade_lut_digest(const GradeLut3D& lut) noexcept {
    GradeLutDigest d;
    if (!lut.valid()) return d;
    const int n = lut.size;

    d.hash = 14695981039346656037ull;
    for (const float v : lut.data) {
        std::uint32_t bits = 0;
        std::memcpy(&bits, &v, sizeof(bits));
        d.hash ^= bits;
        d.hash *= 1099511628211ull;
    }
    std::memcpy(d.mid, &lut.data[(((n / 2) * n + n / 2) * n + n / 2) * 3], sizeof(d.mid));
    std::memcpy(d.black, &lut.data[0], sizeof(d.black));
    std::memcpy(d.white, &lut.data[(((n - 1) * n + (n - 1)) * n + (n - 1)) * 3],
                sizeof(d.white));

    // Skin-tone probe: output at input (0.54, 0.36, 0.31) (R,G,B grid scale),
    // using the same texel-center trilinear coordinates as apply_grade_lut so
    // the digest probe matches what actually renders on a face-toned pixel.
    {
        const auto probe_out = sample_lut_pixel(
            lut, {0.54f, 0.36f, 0.31f});  // (r, g, b) in unit scale
        d.skin[0] = probe_out[0];
        d.skin[1] = probe_out[1];
        d.skin[2] = probe_out[2];
    }

    float worst = 0.0f;
    const float inv = 1.0f / static_cast<float>(n - 1);
    for (int ri = 0; ri < n; ++ri) {
        for (int gi = 0; gi < n; ++gi) {
            for (int bi = 0; bi < n; ++bi) {
                const std::size_t idx =
                    (static_cast<std::size_t>(ri) * static_cast<std::size_t>(n) +
                     static_cast<std::size_t>(gi)) *
                        static_cast<std::size_t>(n) +
                    static_cast<std::size_t>(bi);
                const float in[3] = {static_cast<float>(ri) * inv, static_cast<float>(gi) * inv,
                                     static_cast<float>(bi) * inv};
                for (int c = 0; c < 3; ++c) {
                    worst = std::max(worst, std::fabs(lut.data[idx * 3u + static_cast<std::size_t>(c)] - in[c]));
                }
            }
        }
    }
    d.max_dev = worst;
    return d;
}

VideoFramePtr apply_grade_lut(const VideoFrame& src, const GradeLut3D& lut) {
    if (!lut.valid() || src.width <= 0 || src.height <= 0 || src.rgba.empty())
        return nullptr;

    const int n = lut.size;
    const int last = n - 1;
    const int stride = n * n * 3;

    auto graded = std::make_shared<VideoFrame>();
    graded->pts_ticks = src.pts_ticks;
    graded->pts_seconds = src.pts_seconds;
    graded->frame_number = src.frame_number;
    graded->width = src.width;
    graded->height = src.height;
    graded->stride = src.stride;
    graded->rgba.resize(src.rgba.size());

    const std::size_t pixels = src.rgba.size() / 4u;
    for (std::size_t p = 0; p < pixels; ++p) {
        const std::uint8_t* in = &src.rgba[p * 4u];
        std::uint8_t* out = &graded->rgba[p * 4u];

        // Grid units following the GL texel-center convention: coordinate
        // x*(N-1) centers texel i on input i/(N-1), matching a fragment
        // shader sampling at coord = x*(N-1)/N + 0.5/N with LINEAR filtering.
        const float ur = static_cast<float>(in[0]) * kByteToUnit *
                         static_cast<float>(last);
        const float vg = static_cast<float>(in[1]) * kByteToUnit *
                         static_cast<float>(last);
        const float wb = static_cast<float>(in[2]) * kByteToUnit *
                         static_cast<float>(last);

        int r0 = static_cast<int>(ur);
        int g0 = static_cast<int>(vg);
        int b0 = static_cast<int>(wb);
        r0 = std::clamp(r0, 0, last);
        g0 = std::clamp(g0, 0, last);
        b0 = std::clamp(b0, 0, last);
        const int r1 = std::min(r0 + 1, last);
        const int g1 = std::min(g0 + 1, last);
        const int b1 = std::min(b0 + 1, last);
        const float fr = ur - static_cast<float>(r0);
        const float fg = vg - static_cast<float>(g0);
        const float fb = wb - static_cast<float>(b0);

        // Trilinear over the 8 enclosing grid cells. Indices derive from the
        // (r?,g?,b?) tuples directly (see sample_lut_pixel for why): when
        // r0==last the high neighbor r1 is clamped to last so every read stays
        // in-bounds, and the mirror CUDA kernel (nv12GradeResize) uses the same
        // law bit-for-bit.
        const std::vector<float>& d = lut.data;
        const std::size_t nsz = static_cast<std::size_t>(n);
        const std::size_t i000 = (static_cast<std::size_t>(r0) * nsz + static_cast<std::size_t>(g0)) * nsz +
                                 static_cast<std::size_t>(b0);
        const std::size_t i100 = (static_cast<std::size_t>(r1) * nsz + static_cast<std::size_t>(g0)) * nsz +
                                 static_cast<std::size_t>(b0);
        const std::size_t i001 = (static_cast<std::size_t>(r0) * nsz + static_cast<std::size_t>(g0)) * nsz +
                                 static_cast<std::size_t>(b1);
        const std::size_t i101 = (static_cast<std::size_t>(r1) * nsz + static_cast<std::size_t>(g0)) * nsz +
                                 static_cast<std::size_t>(b1);
        const std::size_t i010 = (static_cast<std::size_t>(r0) * nsz + static_cast<std::size_t>(g1)) * nsz +
                                 static_cast<std::size_t>(b0);
        const std::size_t i110 = (static_cast<std::size_t>(r1) * nsz + static_cast<std::size_t>(g1)) * nsz +
                                 static_cast<std::size_t>(b0);
        const std::size_t i011 = (static_cast<std::size_t>(r0) * nsz + static_cast<std::size_t>(g1)) * nsz +
                                 static_cast<std::size_t>(b1);
        const std::size_t i111 = (static_cast<std::size_t>(r1) * nsz + static_cast<std::size_t>(g1)) * nsz +
                                 static_cast<std::size_t>(b1);

        for (int c = 0; c < 3; ++c) {
            const std::size_t s000 = i000 * 3u + static_cast<std::size_t>(c);
            const std::size_t s100 = i100 * 3u + static_cast<std::size_t>(c);
            const std::size_t s001 = i001 * 3u + static_cast<std::size_t>(c);
            const std::size_t s101 = i101 * 3u + static_cast<std::size_t>(c);
            const std::size_t s010 = i010 * 3u + static_cast<std::size_t>(c);
            const std::size_t s110 = i110 * 3u + static_cast<std::size_t>(c);
            const std::size_t s011 = i011 * 3u + static_cast<std::size_t>(c);
            const std::size_t s111 = i111 * 3u + static_cast<std::size_t>(c);

            // 8-way lerp, R then G then B.
            const float c00 = d[s000] + (d[s001] - d[s000]) * fb;
            const float c10 = d[s100] + (d[s101] - d[s100]) * fb;
            const float c01 = d[s010] + (d[s011] - d[s010]) * fb;
            const float c11 = d[s110] + (d[s111] - d[s110]) * fb;
            const float c0 = c00 + (c01 - c00) * fg;
            const float c1 = c10 + (c11 - c10) * fg;
            out[c] = clamp_unit(c0 + (c1 - c0) * fr);
        }
        out[3] = in[3];
    }
    return graded;
}

}  // namespace canvas::core::grade_graph