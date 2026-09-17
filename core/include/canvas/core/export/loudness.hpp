#pragma once

#include <span>

namespace canvas::core::loudness {

inline constexpr float kSilenceLufs = -70.0f;

[[nodiscard]] float normalization_gain_db(float measured_lufs, float target_lufs) noexcept;

[[nodiscard]] float integrated_loudness_lufs(std::span<const float> mono,
                                             double sample_rate) noexcept;

}
