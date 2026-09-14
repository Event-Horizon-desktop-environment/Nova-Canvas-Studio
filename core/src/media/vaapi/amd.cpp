#include "canvas/core/media/vaapi/amd.hpp"

#include <algorithm>
#include <cctype>

namespace canvas::core::vaapi::amd {

namespace {
const Quirks kQuirks{
    .modifiers_supported = true,
    .force_linear = false,
};

// Case-insensitive substring test. The runtime input is not the bare libva
// driver name ("radeonsi"): it is the whole vaQueryVendorString result, e.g.
// "Mesa Gallium driver 24.3.1 for AMD Radeon RX 7800 XT (radeonsi, LLVM ...)",
// so the driver name must be found anywhere in the string, not just at the
// front.
bool contains_ci(const std::string_view needle, const std::string& hay) noexcept {
    if (needle.empty() || hay.size() < needle.size()) return false;
    for (std::size_t i = 0; i + needle.size() <= hay.size(); ++i) {
        bool eq = true;
        for (std::size_t j = 0; j < needle.size(); ++j) {
            if (std::tolower(static_cast<unsigned char>(needle[j])) !=
                std::tolower(static_cast<unsigned char>(hay[i + j]))) {
                eq = false;
                break;
            }
        }
        if (eq) return true;
    }
    return false;
}
}  // namespace

bool matches(const std::string& driver_name) noexcept {
    return contains_ci(kDriverName, driver_name);
}

const Quirks& quirks() noexcept {
    return kQuirks;
}

}  // namespace canvas::core::vaapi::amd