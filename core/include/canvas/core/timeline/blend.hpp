#pragma once

#include "canvas/core/timeline/model.hpp"

#include <cstdint>

namespace canvas::core::blend {

inline constexpr int kBlendModeCount = 8;

[[nodiscard]] const char* blend_mode_name(BlendMode mode) noexcept;

[[nodiscard]] constexpr bool valid_blend_mode(const BlendMode mode) noexcept {
    const int v = static_cast<int>(mode);
    return v >= 0 && v < kBlendModeCount;
}

[[nodiscard]] uint8_t blend_channel(BlendMode mode, float opacity, uint8_t base,
                                    uint8_t src) noexcept;

}
