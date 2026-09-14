#pragma once

// AMD VAAPI support (mesa radeonsi driver family).
//
// AMD's radeonsi driver exports composed NV12 surfaces via one DRM buffer with
// a single object (multi-plane layout) and reliably reports a real format
// modifier (usually tiled) through VADRMPRIMESurfaceDescriptor. The importer
// should keep modifier passthrough enabled (import with
// EGL_LINUX_DRM_FORMAT_MODIFIER_EXT when the display supports it); radeonsi's
// tiled NV12 imports correctly into EGLImage on the Wayland/X11 EGL backends.
//
// The one historic snag is that some radeonsi/gfx families have reported
// pitch != surface-width alignment for the chroma plane; the per-plane pitch
// from the descriptor is always honored here (never derived from width).

#include "canvas/core/media/vaapi/surface.hpp"

#include <string>
#include <string_view>

namespace canvas::core::vaapi::amd {

inline constexpr std::string_view kDriverName = "radeonsi";

// Per-vendor import behavior for AMD surfaces.
struct Quirks {
    // radeonsi produces a real modifier and its tiled buffers import cleanly.
    bool modifiers_supported = true;
    bool force_linear = false;
};

inline constexpr const char* name() noexcept { return "amd"; }

// True when `driver_name` (the vaQueryVendorString result at export time)
// belongs to the AMD radeonsi/mesa gallium driver family. Case-insensitive
// substring match — the driver name appears inside the full vendor string
// ("Mesa Gallium driver 24.3.1 for AMD Radeon ... (radeonsi, ...)").
[[nodiscard]] bool matches(const std::string& driver_name) noexcept;

[[nodiscard]] const Quirks& quirks() noexcept;

}  // namespace canvas::core::vaapi::amd