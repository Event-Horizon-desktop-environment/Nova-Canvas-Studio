#include "canvas/core/media/vaapi/intel.hpp"

#include <algorithm>
#include <cctype>

namespace canvas::core::vaapi::intel {

namespace {
const Quirks kQuirks{
    .modifiers_supported = true,
    .modifiers_retry_linear = true,
};

// Case-insensitive substring test: the runtime input is the whole
// vaQueryVendorString result, e.g. "Intel iHD driver for Intel(R) UHD
// Graphics 630 - 23.4.5", so a bare prefix comparison misses it.
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
    return contains_ci(kIhdDriverName, driver_name) || contains_ci(kI965DriverName, driver_name);
}

bool is_i965(const std::string& driver_name) noexcept {
    return contains_ci(kI965DriverName, driver_name);
}

const Quirks& quirks() noexcept {
    return kQuirks;
}

}  // namespace canvas::core::vaapi::intel