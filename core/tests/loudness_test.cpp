// Phase 1 (A5) loudness-normalization LAW tests. Pins:
//  - normalization_gain_db: target - measured, clamped to the shared audio_mix
//    volume bounds, with silence (-inf) mapping to the maximum boost;
//  - integrated_loudness_lufs: a full-scale sine reads ~ -3.01 (the block-RMS
//    level), amplitude 0.1 reads ~ -23.01, digital silence reads the -70 floor,
//    and the -10 LU relative gate discards quiet content when loud content
//    dominates the programme.
// Headless — links only canvas_core.

#include "canvas/core/export/loudness.hpp"

#include <cmath>
#include <cstdio>
#include <limits>
#include <numbers>
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

void check_near(const float got, const float want, const float tol, const char* what) {
    if (std::fabs(got - want) > tol) {
        std::fprintf(stderr, "FAIL: %s (got %.4f want %.4f +- %.4f)\n", what,
                     static_cast<double>(got), static_cast<double>(want),
                     static_cast<double>(tol));
        ++failures;
    } else {
        std::printf("ok:   %s\n", what);
    }
}

std::vector<float> sine(const double amplitude, const double seconds, const double sr) {
    std::vector<float> v(static_cast<std::size_t>(seconds * sr));
    for (std::size_t i = 0; i < v.size(); ++i) {
        const double t = static_cast<double>(i) / sr;
        v[i] = static_cast<float>(amplitude *
                                  std::sin(2.0 * std::numbers::pi * 1000.0 * t));
    }
    return v;
}

void test_gain_law() {
    check_near(loudness::normalization_gain_db(-23.0f, -14.0f), 9.0f, 1e-4f,
               "gain to lift -23 to -14");
    check_near(loudness::normalization_gain_db(0.0f, -14.0f), -14.0f, 1e-4f,
               "gain to lower 0 to -14");
    check_near(loudness::normalization_gain_db(-70.0f, -14.0f), 24.0f, 1e-4f,
               "huge boost clamps to kMaxVolumeDb");
    check_near(loudness::normalization_gain_db(1000.0f, -14.0f), -100.0f, 1e-4f,
               "huge cut clamps to kMinVolumeDb");
    check_near(loudness::normalization_gain_db(-std::numeric_limits<float>::infinity(), -14.0f),
               24.0f, 1e-4f, "silence (-inf) -> max boost");
    check_near(loudness::normalization_gain_db(-14.0f, -14.0f), 0.0f, 1e-4f,
               "already at target -> 0 dB");
}

void test_integrated() {
    constexpr double kSr = 48000.0;

    check(loudness::integrated_loudness_lufs({}, kSr) == loudness::kSilenceLufs,
          "empty span -> silence floor");
    const std::span<const float> empty;
    check(loudness::integrated_loudness_lufs(empty, 0.0) == loudness::kSilenceLufs,
          "invalid sample rate -> silence floor");

    const auto full = sine(1.0, 3.0, kSr);
    const float full_lu = loudness::integrated_loudness_lufs(full, kSr);
    check_near(full_lu, -3.0103f, 0.15f, "full-scale 1kHz sine ~ -3.01 LUFS");

    const auto tenth = sine(0.1, 3.0, kSr);
    const float tenth_lu = loudness::integrated_loudness_lufs(tenth, kSr);
    check_near(tenth_lu, -23.0103f, 0.15f, "-20 dB sine ~ -23.01 LUFS");

    std::vector<float> silence(static_cast<std::size_t>(2.0 * kSr), 0.0f);
    check(loudness::integrated_loudness_lufs(silence, kSr) == loudness::kSilenceLufs,
          "digital silence -> -70 floor");

    // Relative gating: 1 s of very quiet content (-63 LUFS, above the -70
    // absolute gate) followed by 3 s of full-scale sine. The quiet content is
    // >10 LU below the programme mean, so it is discarded and the result tracks
    // the loud content (~-3.01, allowing a small straddle-block pull-down).
    std::vector<float> mixed = sine(0.001, 1.0, kSr);
    const auto loud = sine(1.0, 3.0, kSr);
    mixed.insert(mixed.end(), loud.begin(), loud.end());
    const float mixed_lu = loudness::integrated_loudness_lufs(mixed, kSr);
    check(std::fabs(mixed_lu - (-3.0103f)) <= 0.7f,
          "quiet content relative-gated out");
    check(mixed_lu > -10.0f, "mixed loudness is not dragged toward the quiet level");
}

}  // namespace

int main() {
    test_gain_law();
    test_integrated();

    if (failures == 0) {
        std::printf("ALL TESTS PASSED\n");
        return 0;
    }
    std::fprintf(stderr, "%d FAILURES\n", failures);
    return 1;
}
