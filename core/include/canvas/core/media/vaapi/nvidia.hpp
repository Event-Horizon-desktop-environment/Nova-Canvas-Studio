#pragma once

// NVIDIA VAAPI support (the third-party NVIDIA VAAPI adapter driver).
//
// NVIDIA has no first-party VAAPI driver at all (decode or encode — not in the
// 570/580/590 series); the only existing implementation is the community
// `nvidia-vaapi-driver` by elFarto, a decode-only adapter that speaks NVDEC
// via VDPAU interop with no CUDA dependency. Its libva driver name is
// `nvidia` (libnvidia_drv_video.so). It exports dmabufs whose layout is
// LINEAR and whose planes are conventionally packed — they can be imported
// through the same EGLImage path with modifiers forced off, which is exactly
// what the shared importer does.
//
// NOTE: on this project CUDA remains the DEFAULT and preferred path for NVIDIA
// hardware (decode_to_hw already hands a device frame to the NV12 composite
// kernel with zero download). The VAAPI backend exists here so a user who has
// *some* NVIDIA-adjacent setup without working CUDA (or who prefers one GL
// import path everywhere) can opt in; it is not probed ahead of CUDA.

#include "canvas/core/media/vaapi/surface.hpp"

#include <string>
#include <string_view>

namespace canvas::core::vaapi::nvidia {

inline constexpr std::string_view kDriverName = "nvidia";
// The adapter's vaQueryVendorString self-identifies as "VA-API NVDEC driver
// [direct backend]" — it carries the "NVDEC" token, not "nvidia".
inline constexpr std::string_view kNvdecToken = "nvdec";

struct Quirks {
    // The nvidia adapter exports LINEAR buffers; never pass modifier attrs.
    bool modifiers_supported = false;
    bool force_linear = true;
};

inline constexpr const char* name() noexcept { return "nvidia"; }

// True for the `nvidia` driver name (case-insensitive substring) or the
// adapter's "NVDEC" self-identification in the full vendor string.
[[nodiscard]] bool matches(const std::string& driver_name) noexcept;

[[nodiscard]] const Quirks& quirks() noexcept;

}  // namespace canvas::core::vaapi::nvidia