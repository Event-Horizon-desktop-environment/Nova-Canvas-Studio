#include "canvas/core/media/vaapi/driver.hpp"

#include "canvas/core/media/vaapi/amd.hpp"
#include "canvas/core/media/vaapi/intel.hpp"
#include "canvas/core/media/vaapi/nvidia.hpp"

#include <algorithm>
#include <cctype>

namespace canvas::core::vaapi {

namespace {

const ImportPolicy kGeneric{
    .modifiers_supported = true,
    .force_linear = false,
    .modifiers_retry_linear = true,
};
const ImportPolicy kAmd{
    .modifiers_supported = amd::quirks().modifiers_supported,
    .force_linear = amd::quirks().force_linear,
    .modifiers_retry_linear = false,
};
const ImportPolicy kIntel{
    .modifiers_supported = intel::quirks().modifiers_supported,
    .force_linear = false,
    .modifiers_retry_linear = intel::quirks().modifiers_retry_linear,
};
const ImportPolicy kNvidia{
    .modifiers_supported = nvidia::quirks().modifiers_supported,
    .force_linear = nvidia::quirks().force_linear,
    .modifiers_retry_linear = false,
};

}  // namespace

Vendor identify_vendor(const std::string& driver_name) noexcept {
    if (amd::matches(driver_name)) return Vendor::Amd;
    if (intel::matches(driver_name)) return Vendor::Intel;
    if (nvidia::matches(driver_name)) return Vendor::Nvidia;
    return Vendor::Unknown;
}

const ImportPolicy& import_policy(const Vendor vendor) noexcept {
    switch (vendor) {
        case Vendor::Amd: return kAmd;
        case Vendor::Intel: return kIntel;
        case Vendor::Nvidia: return kNvidia;
        default: return kGeneric;
    }
}

}  // namespace canvas::core::vaapi