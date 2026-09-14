#pragma once

// Intel VAAPI support (iHD + legacy i965 driver families).
//
// Intel drivers report two distinct driver names: `iHD` (the modern
// intel-media-driver, used on everything since gen9, including the UHD 7xx
// and Arc series) and `i965` (the legacy driver on older gens). Both use the
// I915_FORMAT_MOD_Y_TILED_* family of modifiers for NV12.
//
// The classic trap: Y-tiled NV12 imports into EGLImage only via the planar
// SPLIT described in the header — the full NV12 image with Y-tiling is
// rejected by EGL (`EGL_BAD_PARAMETER`) on several Mesa versions. Importing
// plane[0] as R8 and plane[1] as RG88 with the *same* tiled modifier works,
// because each is then a single-plane tiled image. So the Intel policy is:
// keep modifiers, but always use the split-plane import (which the shared
// importer already does), and never try a single-image full-NV12 import.
//
// i965 additionally tends to advertise LINEAR while actually handing back
// tiled buffers via the DRM PRIME path confusion in some kernel/Mesa
// pairings; detecting that would need a readback probe, so i965 is treated as
// "prefer modifiers, and if an import fails once, force LINEAR for the rest
// of the session" (the retry is handled by the shared importer).

#include "canvas/core/media/vaapi/surface.hpp"

#include <string>
#include <string_view>

namespace canvas::core::vaapi::intel {

inline constexpr std::string_view kIhdDriverName = "iHD";
inline constexpr std::string_view kI965DriverName = "i965";

struct Quirks {
    // iHD reports real tiled modifiers and imports them split-plane fine.
    bool modifiers_supported = true;
    // i965: attempt modifiers once, then fall back to LINEAR on any failure.
    bool modifiers_retry_linear = true;
};

inline constexpr const char* name() noexcept { return "intel"; }

// True for both the iHD and the legacy i965 driver (case-insensitive
// substring: "Intel iHD driver for Intel(R) UHD ..." contains "iHD").
[[nodiscard]] bool matches(const std::string& driver_name) noexcept;
// Which Intel driver this is — used for logging only.
[[nodiscard]] bool is_i965(const std::string& driver_name) noexcept;

[[nodiscard]] const Quirks& quirks() noexcept;

}  // namespace canvas::core::vaapi::intel