#include "canvas/core/export/loudness.hpp"

#include "canvas/core/timeline/audio_mix.hpp"

#include <algorithm>
#include <cmath>
#include <vector>

namespace canvas::core::loudness {

namespace {
constexpr double kEnergyFloor = 1e-10;
}

float normalization_gain_db(const float measured_lufs, const float target_lufs) noexcept {
    float gain = target_lufs - measured_lufs;
    if (!std::isfinite(gain)) gain = audio_mix::kMaxVolumeDb;
    return audio_mix::normalize_volume_db(gain);
}

float integrated_loudness_lufs(const std::span<const float> mono,
                               const double sample_rate) noexcept {
    if (mono.empty() || sample_rate <= 0.0) return kSilenceLufs;

    const auto block = std::max<int64_t>(
        1, static_cast<int64_t>(std::llround(0.4 * sample_rate)));
    const auto hop =
        std::max<int64_t>(1, static_cast<int64_t>(std::llround(0.1 * sample_rate)));
    if (static_cast<int64_t>(mono.size()) < block) return kSilenceLufs;

    const auto lu_of = [](const double mean_sq) {
        return 10.0 * std::log10(std::max(mean_sq, kEnergyFloor));
    };

    std::vector<double> means;
    for (std::size_t start = 0; start + static_cast<std::size_t>(block) <= mono.size();
         start += static_cast<std::size_t>(hop)) {
        double sum = 0.0;
        for (std::size_t i = start; i < start + static_cast<std::size_t>(block); ++i) {
            const double s = mono[i];
            sum += s * s;
        }
        means.push_back(sum / static_cast<double>(block));
    }
    if (means.empty()) return kSilenceLufs;

    std::vector<double> gated;
    for (const double m : means)
        if (lu_of(m) >= kSilenceLufs) gated.push_back(m);
    if (gated.empty()) return kSilenceLufs;

    double sum = 0.0;
    for (const double m : gated) sum += m;
    const double abs_mean = sum / static_cast<double>(gated.size());
    const double rel_threshold = lu_of(abs_mean) - 10.0;

    double kept_sum = 0.0;
    std::size_t kept = 0;
    for (const double m : gated) {
        if (lu_of(m) >= rel_threshold) {
            kept_sum += m;
            ++kept;
        }
    }
    if (kept == 0) return kSilenceLufs;
    return static_cast<float>(lu_of(kept_sum / static_cast<double>(kept)));
}

}
