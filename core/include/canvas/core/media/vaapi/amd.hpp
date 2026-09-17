#pragma once

#include "canvas/core/media/vaapi/surface.hpp"

#include <string>
#include <string_view>

namespace canvas::core::vaapi::amd {

inline constexpr std::string_view kDriverName = "radeonsi";

struct Quirks {
    bool modifiers_supported = true;
    bool force_linear = false;
};

inline constexpr const char* name() noexcept { return "amd"; }

[[nodiscard]] bool matches(const std::string& driver_name) noexcept;

[[nodiscard]] const Quirks& quirks() noexcept;

}
