#include "canvas/core/media/vaapi/nvidia.hpp"

#include <algorithm>
#include <cctype>

namespace canvas::core::vaapi::nvidia {

namespace {
const Quirks kQuirks{
    .modifiers_supported = false,
    .force_linear = true,
};

// Case-insensitive substring test: the runtime input is the whole
// vaQueryVendorString result. The adapter self-identifies as "VA-API NVDEC
// driver [direct backend]" (an "NVDEC" token, not "nvidia"); older builds
// printed "NVIDIA VA-API ... (nvidia)". Accept both tokens.
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
    return contains_ci(kDriverName, driver_name) || contains_ci(kNvdecToken, driver_name);
}

const Quirks& quirks() noexcept {
    return kQuirks;
}

}  // namespace canvas::core::vaapi::nvidia