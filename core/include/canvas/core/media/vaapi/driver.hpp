#pragma once

// VAAPI driver registry: map a libva driver identity to a vendor + behavior.
//
// The decode worker learns the driver once (vaQueryDriverName / vendor string
// at VA display setup) and asks this registry for:
//   - which vendor module owns it (amd/intel/nvidia), for logging + policy
//   - the vendor's import quirks (modifier support, linear fallback rules)
//
// Every vendor's knowledge lives in its own amd/intel/nvidia module; this
// header only orchestrates. A driver this registry has never heard of resolves
// to Vendor::Unknown with the conservative "assume LINEAR, allow modifiers"
// defaults — the same rules a skip-free build on a brand-new driver family
// needs (an import that fails falls back to the CPU path, never crashes).

#include "canvas/core/media/vaapi/surface.hpp"

#include <string>

namespace canvas::core::vaapi {

// Union of what the import path needs to know about a vendor, assembled from
// the per-vendor modules. Kept intentionally small.
struct ImportPolicy {
    bool modifiers_supported = true;      // pass EGL_LINUX_DRM_FORMAT_MODIFIER_EXT
    bool force_linear = false;            // import with no modifier regardless
    bool modifiers_retry_linear = false;  // on failure, retry planes as LINEAR
};

// Resolves a libva driver name (vaQueryDriverName result, e.g. "radeonsi",
// "iHD", "nvidia") to a vendor. Case-insensitive.
[[nodiscard]] Vendor identify_vendor(const std::string& driver_name) noexcept;

// Import behavior for a vendor (assembled from the per-vendor modules).
[[nodiscard]] const ImportPolicy& import_policy(Vendor vendor) noexcept;

}  // namespace canvas::core::vaapi