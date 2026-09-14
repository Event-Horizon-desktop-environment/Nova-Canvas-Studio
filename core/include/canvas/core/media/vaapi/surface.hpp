#pragma once

// Zero-copy VAAPI surface handle.
//
// A decoded VAAPI NV12 frame lives in a DRM object (or two) on the GPU. This
// module models that surface as plain data — the exported dmabuf fds plus the
// per-plane offsets/pitches the GL importer needs — with NO libva or Qt types
// in the header, so it is usable from headless core code, the decoder worker,
// and the GL viewer alike.
//
// Ownership is the point of this type:
//   - `objects[].fd` is a dup() of the driver's exported fd. The VaapiSurface
//     owns it and closes it on destruction (or move-transfer).
//   - `pin` is an opaque ref-counted ref (an ffmpeg AVBufferRef underneath)
//     that keeps the source *VA surface* reserved in its pool while this
//     surface lives. Without it the VA pool can recycle the VASurfaceID as
//     soon as the decoder reuses `av_frame_`, aliasing the pixels under a live
//     GL texture. The pin is what makes moving the exported surface to the GL
//     thread safe.
//
// The GL side (gui vaapi_viewer) imports each plane via EGLImage
// (EGL_LINUX_DRM_FOURCC_EXT): plane[0] as a R8 image, plane[1] as a RG88
// image, both referencing the same DRM object fd with different offsets.

#include "canvas/core/gpu/colorspace.hpp"

#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

namespace canvas::core::vaapi {

// DRM_FORMAT_NV12 — the composite fourcc of the exported surface.
inline constexpr std::uint32_t kDrmFourccNv12 = 0x3231564Eu;  // 'N','V','1','2' (little-endian)
// DRM_FORMAT_R8 — luma plane imported alone (EGL_LINUX_DRM_FOURCC_EXT).
inline constexpr std::uint32_t kDrmFourccR8 = 0x20203852u;    // 'R','8',' ',' '
// DRM_FORMAT_RG88 — interleaved chroma plane imported alone.
inline constexpr std::uint32_t kDrmFourccRg88 = 0x38384752u;  // 'R','G','8','8'

// Which VAAPI driver (family) produced this surface. Surfaced from the driver
// string at export time so the importer can pick vendor-tuned behavior without
// re-probing. `Unknown` is treated as a generic/linear import.
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

// One DRM object backing the exported surface. `fd` is owned (closed by the
// VaapiSurface destructor / moved out on move). `modifier` is the DRM format
// modifier of the buffer; LINEAR (0) needs no modifier attrs on the import.
struct VaapiObject {
    int fd = -1;
    std::uint64_t modifier = 0;
};

// One plane within an object: which object, the byte offset into it, and the
// row pitch. The GL importer needs exactly these three numbers per plane.
struct VaapiPlane {
    std::uint32_t object_index = 0;
    std::uint32_t offset = 0;
    std::uint32_t pitch = 0;
};

// Owning, move-only description of an exported VAAPI NV12 surface. Copies are
// deleted so a fd can never be double-closed; a (shared, removal-safe) shared_ptr
// wrapper is used across threads instead (VaapiSurfacePtr).
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
    // Opaque reservation of the source VA surface (an ffmpeg AVBufferRef). See
    // the file-top rationale. Null when no pin was obtainable; the surface is
    // still safe to import, just not guaranteed stable across decoder reuse.
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

}  // namespace canvas::core::vaapi