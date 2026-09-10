// Phase LUT tests for the Resolve-style grade flattening
// (canvas/core/grade_graph/lut.hpp). Exercises the bake+trilinear contract:
// identity reproduces input byte-exactly, the grid matches the reference
// per-pixel evaluator, affine maps survive trilinear interpolation exactly,
// inactive trees produce a null LUT (passthrough), and 2x2 byte-exact
// half-gain round-trips mirror test_apply_grade_to_frame. Headless — links
// only canvas_core.

#include "canvas/core/grade_graph/graph.hpp"
#include "canvas/core/grade_graph/eval.hpp"
#include "canvas/core/grade_graph/lut.hpp"
#include "canvas/core/export/grade_frame.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

namespace gg = canvas::core::grade_graph;

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

// Wire a single LGG corrector (gamma=1 => out = gain * in) into the terminal.
int add_gain_tree(gg::GradeGraph& g, float gain) {
    const int corr = g.add_node(gg::NodeKind::kCorrector);
    g.node(corr).correct_mode = gg::CorrectMode::kLgg;
    g.node(corr).lgg.emplace();
    g.node(corr).lgg->gain_master = gain;
    const int out_n = g.add_node(gg::NodeKind::kOutput);
    if (g.add_rgb_edge(corr, out_n) < 0) return -1;
    return out_n;
}

void test_identity_grid_roundtrip() {
    // Identity tree (gain 1.0): the LUT must reproduce input byte-exactly.
    gg::GradeGraph g;
    (void)add_gain_tree(g, 1.0f);

    const gg::GradeLutPtr lut = gg::bake_grade_lut(g, 33);
    check(lut != nullptr && lut->valid(), "lut: identity bakes a valid LUT");

    canvas::core::VideoFrame src;
    src.width = 4;
    src.height = 1;
    src.stride = 16;
    src.rgba = {0, 0, 0, 255, 64, 128, 192, 255, 130, 40, 220, 255, 255, 255, 255, 255};

    canvas::core::VideoFramePtr out = gg::apply_grade_lut(src, *lut);
    check(out != nullptr, "lut: apply produces a frame");
    if (out) {
        bool ok = out->rgba[0] == 0u && out->rgba[7] == 255u && out->rgba[8] == 130u &&
                  out->rgba[9] == 40u && out->rgba[10] == 220u && out->rgba[15] == 255u;
        check(ok, "lut: identity reproduces input byte-exactly");
        check(out->rgba[3] == 255u, "lut: alpha rides along");
        check(out->width == 4 && out->height == 1 && out->stride == 16,
              "lut: geometry preserved");
    }
}

void test_half_gain_byte_exact() {
    // Gain 0.5 is affine; trilinear reproduces affine maps exactly on the grid,
    // so the same byte-exact values as test_apply_grade_to_frame must hold.
    gg::GradeGraph g;
    (void)add_gain_tree(g, 0.5f);

    const gg::GradeLutPtr lut = gg::bake_grade_lut(g, 17);
    check(lut != nullptr, "lut: half-gain bakes");

    canvas::core::VideoFrame src;
    src.width = 2;
    src.height = 2;
    src.stride = 8;
    src.rgba = {40, 80, 160, 255, 10, 20, 30, 255, 90, 140, 200, 255, 0, 128, 255, 255};

    canvas::core::VideoFramePtr graded = gg::apply_grade_lut(src, *lut);
    check(graded != nullptr, "lut: half-gain frame produced");
    if (graded) {
        check(graded->rgba[0] == 20u && graded->rgba[1] == 40u && graded->rgba[2] == 80u,
              "lut: gain 0.5 halves RGB bytes");
        check(graded->rgba[3] == 255u, "lut: alpha rides along");
        check(graded->rgba[12] == 0u && graded->rgba[13] == 64u,
              "lut: byte-exact 0/128 -> 0/64");
    }
}

void test_grid_matches_evaluator() {
    // Non-affine-ish LGG gamma pull: the LUT grid must track the reference
    // evaluator at/behind gridpoints (trilinear is exact on the grid itself).
    gg::GradeGraph g;
    const int corr = g.add_node(gg::NodeKind::kCorrector);
    g.node(corr).correct_mode = gg::CorrectMode::kLgg;
    g.node(corr).lgg.emplace();
    g.node(corr).lgg->gamma_master = 0.7f;
    const int out_n = g.add_node(gg::NodeKind::kOutput);
    check(g.add_rgb_edge(corr, out_n) >= 0, "lut: wire contrast tree");

    const int grid_size = 33;
    const gg::GradeLutPtr lut = gg::bake_grade_lut(g, grid_size);
    check(lut != nullptr, "lut: contrast bakes");

    // Small 3x1 probe; 16-pixel stride mirrors the GL texture sampling where
    // in=0 and in=255 land exactly on gridpoint 0 / 31.
    canvas::core::VideoFrame src;
    src.width = 3;
    src.height = 1;
    src.stride = 12;
    src.rgba = {0, 0, 0, 255, 96, 96, 96, 255, 255, 255, 255, 255};

    canvas::core::VideoFramePtr lut_out = gg::apply_grade_lut(src, *lut);
    canvas::core::VideoFramePtr ref = canvas::core::apply_grade_to_frame(src, g);
    check(lut_out != nullptr && ref != nullptr, "lut: both apply paths produce frames");
    if (lut_out && ref) {
        bool ok = true;
        for (std::size_t i = 0; i < src.rgba.size(); i += 4) {
            for (int c = 0; c < 3; ++c) {
                const int a = lut_out->rgba[i + static_cast<std::size_t>(c)];
                const int b = ref->rgba[i + static_cast<std::size_t>(c)];
                if (a != b) ok = false;  // gridpoints mid-byte can land on a neighbor cell
            }
        }
        check(ok, "lut: grid output matches reference evaluator byte-for-byte");
    }
}

void test_inactive_and_empty() {
    canvas::core::VideoFrame src;
    src.width = 1;
    src.height = 1;
    src.stride = 4;
    src.rgba = {100, 100, 100, 255};

    // Unwired tree: no output node reaches the terminal => no grade => null.
    gg::GradeGraph bare;
    (void)bare.add_node(gg::NodeKind::kCorrector);
    check(gg::bake_grade_lut(bare, 33) == nullptr, "lut: unwired tree bakes null");

    // Empty graph (the "no grade" reporter state) is passthrough.
    check(gg::bake_grade_lut(gg::GradeGraph{}, 33) == nullptr,
          "lut: empty graph bakes null");

    // Invalid LUT (size 0 / empty data) must never crash: passthrough contract.
    check(gg::apply_grade_lut(src, gg::GradeLut3D{}) == nullptr,
          "lut: apply on empty LUT returns nullptr");
}

void test_size_two_endpoints() {
    // A size-2 LUT has exactly two gridpoints (0 and 1). Affine gain must hit
    // them; mid-values interpolate linearly (still exact for affine).
    gg::GradeGraph g;
    (void)add_gain_tree(g, 2.0f);

    const gg::GradeLutPtr lut = gg::bake_grade_lut(g, 2);
    check(lut != nullptr && lut->valid(), "lut: size-2 bakes valid");

    canvas::core::VideoFrame src;
    src.width = 2;
    src.height = 1;
    src.stride = 8;
    src.rgba = {0, 64, 128, 255, 128, 255, 255, 255};

    canvas::core::VideoFramePtr graded = gg::apply_grade_lut(src, *lut);
    check(graded != nullptr, "lut: size-2 applies");
    if (graded) {
        // gain 2.0: 0->0, 64->128, 128->255 (clamped), 255->255 (clamped)
        check(graded->rgba[0] == 0u && graded->rgba[1] == 128u && graded->rgba[2] == 255u,
              "lut: size-2 affine half-grid");
        check(graded->rgba[4] == 255u && graded->rgba[5] == 255u && graded->rgba[6] == 255u,
              "lut: size-2 clamps at top");
    }
}

void test_offset_byte_exact() {
    // Offset wheel's additive term (applied before the LGG stage) is affine, so
    // trilinear must reproduce it exactly across the grid, matching the
    // reference evaluator byte-for-byte.
    gg::GradeGraph g;
    const int corr = g.add_node(gg::NodeKind::kCorrector);
    g.node(corr).correct_mode = gg::CorrectMode::kLgg;
    g.node(corr).lgg.emplace();
    g.node(corr).lgg->gain_master = 0.5f;
    g.node(corr).offset.emplace();
    g.node(corr).offset->master = 0.15f;
    g.node(corr).offset->r = 0.5f;
    g.node(corr).offset->g = -0.25f;
    g.node(corr).offset->b = 0.1f;
    const int out_n = g.add_node(gg::NodeKind::kOutput);
    check(g.add_rgb_edge(corr, out_n) >= 0, "lut: wire offset tree");

    const gg::GradeLutPtr lut = gg::bake_grade_lut(g, 33);
    check(lut != nullptr, "lut: offset bakes");

    canvas::core::VideoFrame src;
    src.width = 3;
    src.height = 1;
    src.stride = 12;
    src.rgba = {0, 0, 0, 255, 96, 96, 96, 255, 255, 255, 255, 255};

    canvas::core::VideoFramePtr lut_out = gg::apply_grade_lut(src, *lut);
    canvas::core::VideoFramePtr ref = canvas::core::apply_grade_to_frame(src, g);
    check(lut_out != nullptr && ref != nullptr, "lut: offset both apply paths produce frames");
    if (lut_out && ref) {
        bool ok = true;
        for (std::size_t i = 0; i < src.rgba.size(); i += 4) {
            for (int c = 0; c < 3; ++c) {
                const int a = lut_out->rgba[i + static_cast<std::size_t>(c)];
                const int b = ref->rgba[i + static_cast<std::size_t>(c)];
                if (a != b) ok = false;
            }
        }
        check(ok, "lut: offset grid output matches reference evaluator byte-for-byte");
    }

    // Explicit channel-order check: offset.r lifts red at black (master 0.15 +
    // r 0.5 then *0.5 gain) while offset.g (-0.25) pins green at 0.
    if (lut_out) {
        check(lut_out->rgba[0] > 0u, "lut: offset red lifts at black");
        check(lut_out->rgba[1] == 0u, "lut: offset green pinned at black");
        check(lut_out->rgba[2] > 0u, "lut: offset blue lifts at black");
    }
}

// GPU-vs-CPU orientation contract: bake_grade_lut writes the grid R-MAJOR —
// grid point (r,g,b) occupies data[((r*N)+g)*N + b] (r slowest index, b
// fastest) — but the viewer uploads that array to a GL 3D texture VERBATIM,
// where x is the FASTEST axis. A texel at (x,y,z) therefore reads
// data[x + y*N + z*N*N], i.e. gridpoint (r=z, g=y, b=x): a naive sampling
// coordinate `coord = rgb*...` evaluates the LUT with R and B exchanged —
// the "blue skin during full-res playback while scrub (CPU path) is correct"
// bug. The NV12 shaders (grade_rgb in viewer_gl.cpp) compensate by driving
// the texture's x axis with the B input and the z axis with the R input. This
// test models the GL x-fastest sample of the r-major array and asserts the
// swapped coordinate law reproduces the CPU reference, while the unswapped
// law does not (so the R/B swap cannot regress silently).
void test_gpu_lut_orientation() {
    const int n = 6;
    gg::GradeLut3D lut;
    lut.size = n;
    lut.data.assign(static_cast<std::size_t>(n) * static_cast<std::size_t>(n) *
                        static_cast<std::size_t>(n) * 3u,
                    0.0f);
    // Marker LUT: each grid point stores a value that changes monotonically
    // along r (slowest), g, and b (fastest). Any R/B mixup in the sample
    // coordinate then shows up as a large deviation.
    for (int ri = 0; ri < n; ++ri)
        for (int gi = 0; gi < n; ++gi)
            for (int bi = 0; bi < n; ++bi) {
                const std::size_t idx = (static_cast<std::size_t>(ri) * n +
                                         static_cast<std::size_t>(gi)) *
                                            static_cast<std::size_t>(n) +
                                        static_cast<std::size_t>(bi);
                lut.data[idx * 3u + 0u] = static_cast<float>(ri) / static_cast<float>(n - 1);
                lut.data[idx * 3u + 1u] = static_cast<float>(gi) / static_cast<float>(n - 1);
                lut.data[idx * 3u + 2u] = static_cast<float>(bi) / static_cast<float>(n - 1);
            }
    check(lut.valid(), "lut: synthetic orientation grid valid");

    const auto cl = [](float v) { return std::clamp(v, 0.0f, 1.0f); };
    // Module sampling convention: sequential-access grid coordinate u = x*(N-1)
    // (lut.hpp). An input of 1.0 lands on the last gridpoint N-1, so the marker
    // (identity ramp) is reconstructed exactly regardless of interpolation.
    const float grid_coord = static_cast<float>(n - 1);

    // CPU reference: the apply_grade_lut law, reading data[((r*N)+g)*N + b].
    auto cpu_ref = [&](float r, float g, float b) -> std::array<float, 3> {
        const float ur = cl(r) * grid_coord, vg = cl(g) * grid_coord, wb = cl(b) * grid_coord;
        const int r0 = static_cast<int>(ur), g0 = static_cast<int>(vg), b0 = static_cast<int>(wb);
        const int r1 = std::min(r0 + 1, n - 1), g1 = std::min(g0 + 1, n - 1),
                  b1 = std::min(b0 + 1, n - 1);
        const float fr = ur - r0, fg = vg - g0, fb = wb - b0;
        auto at = [&](int ri, int gi, int bi) -> const float* {
            return &lut.data[(static_cast<std::size_t>((ri * n) + gi) * n + bi) * 3u];
        };
        std::array<float, 3> out{};
        for (int c = 0; c < 3; ++c) {
            const float c00 = at(r0, g0, b0)[c] + (at(r0, g0, b1)[c] - at(r0, g0, b0)[c]) * fb;
            const float c10 = at(r1, g0, b0)[c] + (at(r1, g0, b1)[c] - at(r1, g0, b0)[c]) * fb;
            const float c01 = at(r0, g1, b0)[c] + (at(r0, g1, b1)[c] - at(r0, g1, b0)[c]) * fb;
            const float c11 = at(r1, g1, b0)[c] + (at(r1, g1, b1)[c] - at(r1, g1, b0)[c]) * fb;
            const float c0 = c00 + (c01 - c00) * fg;
            const float c1 = c10 + (c11 - c10) * fg;
            out[c] = c0 + (c1 - c0) * fr;
        }
        return out;
    };

    // GL model: the r-major array is typed into a 3D texture with x fastest, so
    // texel (x,y,z) = data[x + y*N + z*N*N] = gridpoint (r=z, g=y, b=x). The
    // shader samples the texel-center coordinate scaled by (N-1)/N + 0.5/N; in
    // grid units that is exactly `channel*(N-1)` per axis. swap_rb models the
    // shader driving x with the B input / z with the R input (fixed) vs. the
    // naive x=R / z=B (the R/B-mixed law that broke full-res playback).
    auto gl_sample = [&](float r, float g, float b, bool swap_rb) -> std::array<float, 3> {
        const float xg = swap_rb ? cl(b) * grid_coord : cl(r) * grid_coord;
        const float yg = cl(g) * grid_coord;
        const float zg = swap_rb ? cl(r) * grid_coord : cl(b) * grid_coord;
        const int x0 = static_cast<int>(xg), y0 = static_cast<int>(yg), z0 = static_cast<int>(zg);
        const int x1 = std::min(x0 + 1, n - 1), y1 = std::min(y0 + 1, n - 1),
                  z1 = std::min(z0 + 1, n - 1);
        const float fx = xg - x0, fy = yg - y0, fz = zg - z0;
        auto at = [&](int xi, int yi, int zi) -> const float* {
            return &lut.data[(static_cast<std::size_t>(xi + yi * n + zi * n * n)) * 3u];
        };
        std::array<float, 3> out{};
        for (int c = 0; c < 3; ++c) {
            const float c00 = at(x0, y0, z0)[c] + (at(x0, y0, z1)[c] - at(x0, y0, z0)[c]) * fz;
            const float c10 = at(x1, y0, z0)[c] + (at(x1, y0, z1)[c] - at(x1, y0, z0)[c]) * fz;
            const float c01 = at(x0, y1, z0)[c] + (at(x0, y1, z1)[c] - at(x0, y1, z0)[c]) * fz;
            const float c11 = at(x1, y1, z0)[c] + (at(x1, y1, z1)[c] - at(x1, y1, z0)[c]) * fz;
            const float c0 = c00 + (c01 - c00) * fy;
            const float c1 = c10 + (c11 - c10) * fy;
            out[c] = c0 + (c1 - c0) * fx;
        }
        return out;
    };

    const std::array<std::array<float, 3>, 6> probes = {
        std::array<float, 3>{0.95f, 0.14f, 0.08f},  // strong R/B asymmetry -> naive law diverges
        std::array<float, 3>{0.50f, 0.50f, 0.50f},  // gray: swap-invariant, must still match
        std::array<float, 3>{0.08f, 0.60f, 0.90f},
        std::array<float, 3>{0.31f, 0.47f, 0.12f},
        std::array<float, 3>{0.29f, 0.30f, 0.95f},
        std::array<float, 3>{0.77f, 0.33f, 0.55f},
    };
    float max_fixed_dev = 0.0f;
    float max_naive_dev = 0.0f;
    for (const auto& p : probes) {
        const auto ref = cpu_ref(p[0], p[1], p[2]);
        const auto fixed = gl_sample(p[0], p[1], p[2], true);
        const auto naive = gl_sample(p[0], p[1], p[2], false);
        for (int c = 0; c < 3; ++c) {
            max_fixed_dev = std::max(max_fixed_dev, std::fabs(fixed[c] - ref[c]));
            max_naive_dev = std::max(max_naive_dev, std::fabs(naive[c] - ref[c]));
        }
    }
    check(max_fixed_dev < 1e-4f,
          "lut: GPU R/B-swapped coordinate matches the CPU reference");
    check(max_naive_dev > 0.05f,
          "lut: naive (unswapped) GPU coordinate diverges from CPU — swap caught");
}

}  // namespace

int main() {
    test_identity_grid_roundtrip();
    test_half_gain_byte_exact();
    test_grid_matches_evaluator();
    test_inactive_and_empty();
    test_size_two_endpoints();
    test_offset_byte_exact();
    test_gpu_lut_orientation();

    if (failures == 0) {
        std::printf("ALL TESTS PASSED\n");
        return 0;
    }
    std::fprintf(stderr, "%d FAILURES\n", failures);
    return 1;
}