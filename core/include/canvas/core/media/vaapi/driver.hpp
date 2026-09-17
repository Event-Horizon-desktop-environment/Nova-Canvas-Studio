#pragma once

#include "canvas/core/media/vaapi/surface.hpp"

#include <string>

namespace canvas::core::vaapi {

struct ImportPolicy {
    bool modifiers_supported = true;
    bool force_linear = false;
    bool modifiers_retry_linear = false;
};

[[nodiscard]] Vendor identify_vendor(const std::string& driver_name) noexcept;

[[nodiscard]] const ImportPolicy& import_policy(Vendor vendor) noexcept;

}
