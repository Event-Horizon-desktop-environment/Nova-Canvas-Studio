#pragma once

#include "canvas/core/gpu/colorspace.hpp"

#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

namespace canvas::core::vaapi {

inline constexpr std::uint32_t kDrmFourccNv12 = 0x3231564Eu;
inline constexpr std::uint32_t kDrmFourccR8 = 0x20203852u;
inline constexpr std::uint32_t kDrmFourccRg88 = 0x38384752u;

enum class Vendor : std::uint8_t {
    Unknown = 0,
    Amd,
    Intel,
    Nvidia,
};

inline constexpr const char* vendor_name(const Vendor v) noexcept {
    switch (v) {
        case Vendor::Amd: return "amd";
        case Vendor::Intel: return "intel";
        case Vendor::Nvidia: return "nvidia";
        default: return "unknown";
    }
}

struct VaapiObject {
    int fd = -1;
    std::uint64_t modifier = 0;
};

struct VaapiPlane {
    std::uint32_t object_index = 0;
    std::uint32_t offset = 0;
    std::uint32_t pitch = 0;
};

struct VaapiSurface {
    std::uint32_t fourcc = 0;
    int width = 0;
    int height = 0;
    int64_t frame_number = 0;
    gpu::ColorMatrix matrix = gpu::ColorMatrix::BT709;
    gpu::ColorRange range = gpu::ColorRange::Limited;
    Vendor vendor = Vendor::Unknown;
    std::vector<VaapiObject> objects;
    std::vector<VaapiPlane> planes;
    std::shared_ptr<const void> pin;

    VaapiSurface() = default;
    ~VaapiSurface();
    VaapiSurface(const VaapiSurface&) = delete;
    VaapiSurface& operator=(const VaapiSurface&) = delete;
    VaapiSurface(VaapiSurface&& other) noexcept;
    VaapiSurface& operator=(VaapiSurface&& other) noexcept;

    [[nodiscard]] bool valid() const noexcept {
        return fourcc != 0 && !objects.empty() && planes.size() >= 2 && width > 0 && height > 0;
    }
};

using VaapiSurfacePtr = std::shared_ptr<const VaapiSurface>;

}
