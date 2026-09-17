#include "canvas/core/qc/metrics.hpp"

#include <cmath>
#include <cstdio>
#include <cstdint>
#include <limits>
#include <vector>

using namespace canvas::core;

namespace {

int failures = 0;

void check(const bool cond, const char* what) {
    if (!cond) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++failures;
    } else {
        std::printf("ok:   %s\n", what);
    }
}

std::vector<std::uint8_t> fill(const int w, const int h, const std::uint8_t v) {
    return std::vector<std::uint8_t>(static_cast<std::size_t>(w) * h, v);
}

void test_valid() {
    std::vector<std::uint8_t> buf(16, 0);
    check(!qc::valid(qc::Plane{}), "null plane invalid");
    check(!qc::valid(qc::Plane{buf.data(), 0, 4, 0}), "zero width invalid");
    check(!qc::valid(qc::Plane{buf.data(), 4, 0, 4}), "zero height invalid");
    check(!qc::valid(qc::Plane{buf.data(), 4, 4, 2}), "stride < width invalid");
    check(qc::valid(qc::Plane{buf.data(), 4, 4, 4}), "tight plane valid");
}

void test_psnr() {
    constexpr int W = 8, H = 8;

    auto black = fill(W, H, 0);
    auto off1 = fill(W, H, 1);
    auto white = fill(W, H, 255);

    const qc::Plane pb{black.data(), W, H, W};
    const qc::Plane p1{off1.data(), W, H, W};
    const qc::Plane pw{white.data(), W, H, W};

    check(std::isinf(qc::psnr(pb, pb)) && qc::psnr(pb, pb) > 0, "identical -> +inf");

    const double expected = 10.0 * std::log10(255.0 * 255.0);
    check(std::fabs(qc::psnr(pb, p1) - expected) < 1e-9, "uniform +1 -> 10*log10(255^2)");

    check(std::fabs(qc::psnr(pb, pw) - 0.0) < 1e-9, "black vs white -> 0 dB");

    std::vector<std::uint8_t> small(4 * 4, 0);
    const qc::Plane ps{small.data(), 4, 4, 4};
    check(qc::psnr(pb, ps) == 0.0, "mismatched dims -> 0.0");
    check(qc::psnr(qc::Plane{}, pb) == 0.0, "invalid input -> 0.0");
}

void test_ssim() {
    constexpr int W = 16, H = 16;

    auto base = fill(W, H, 100);
    const qc::Plane pbase{base.data(), W, H, W};
    check(std::fabs(qc::ssim(pbase, pbase) - 1.0) < 1e-12, "identical -> 1.0");

    auto other = fill(W, H, 100);
    const qc::Plane pother{other.data(), W, H, W};
    check(std::fabs(qc::ssim(pbase, pother) - 1.0) < 1e-12, "equal constant planes -> 1.0");

    std::vector<std::uint8_t> grad(static_cast<std::size_t>(W) * H);
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x)
            grad[static_cast<std::size_t>(y) * W + x] =
                static_cast<std::uint8_t>((x * 8 + y * 16) & 0xFF);

    double prev = 2.0;
    for (const int amp : {0, 8, 24, 64}) {
        std::vector<std::uint8_t> noisy = grad;
        std::uint32_t seed = 12345u;
        for (auto& v : noisy) {
            seed = seed * 1664525u + 1013904223u;
            const int jitter = static_cast<int>((seed >> 24) % (2 * amp + 1)) - amp;
            int nv = static_cast<int>(v) + jitter;
            nv = nv < 0 ? 0 : (nv > 255 ? 255 : nv);
            v = static_cast<std::uint8_t>(nv);
        }
        const qc::Plane pg{grad.data(), W, H, W};
        const qc::Plane pn{noisy.data(), W, H, W};
        const double s = qc::ssim(pg, pn);
        check(s <= prev + 1e-9, "ssim falls as noise grows");
        prev = s;
    }
    check(prev < 0.9, "heavy noise visibly lowers ssim");

    check(qc::ssim(qc::Plane{}, pbase) == 0.0, "invalid input -> 0.0");
}

void test_stride_safety() {
    constexpr int W = 4, H = 4, STRIDE = 8;
    std::vector<std::uint8_t> a(static_cast<std::size_t>(STRIDE) * H, 0xAA);
    std::vector<std::uint8_t> b(static_cast<std::size_t>(STRIDE) * H, 0x55);
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            const std::size_t i = static_cast<std::size_t>(y) * STRIDE + x;
            a[i] = static_cast<std::uint8_t>(x * 16 + y);
            b[i] = a[i];
        }
    const qc::Plane pa{a.data(), W, H, STRIDE};
    const qc::Plane pb{b.data(), W, H, STRIDE};
    check(std::isinf(qc::psnr(pa, pb)), "padding ignored by psnr (identical visible data)");
    check(std::fabs(qc::ssim(pa, pb) - 1.0) < 1e-12, "padding ignored by ssim");

    b[2] = static_cast<std::uint8_t>(b[2] + 30);
    check(!std::isinf(qc::psnr(pa, pb)), "visible difference detected through stride");
}

}

int main() {
    test_valid();
    test_psnr();
    test_ssim();
    test_stride_safety();

    if (failures == 0) {
        std::printf("ALL TESTS PASSED\n");
        return 0;
    }
    std::fprintf(stderr, "%d FAILURES\n", failures);
    return 1;
}
