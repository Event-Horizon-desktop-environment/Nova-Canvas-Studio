#pragma once

#include <cstdint>

namespace canvas::core::qc {

struct Plane {
    const std::uint8_t* data = nullptr;
    int width = 0;
    int height = 0;
    int stride = 0;
};

[[nodiscard]] bool valid(const Plane& p) noexcept;

[[nodiscard]] double psnr(const Plane& a, const Plane& b) noexcept;

[[nodiscard]] double ssim(const Plane& a, const Plane& b) noexcept;

}
