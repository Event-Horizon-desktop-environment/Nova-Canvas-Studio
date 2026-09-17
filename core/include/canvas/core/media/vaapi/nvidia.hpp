#pragma once

#include "canvas/core/media/vaapi/surface.hpp"

#include <string>
#include <string_view>

namespace canvas::core::vaapi::nvidia {

inline constexpr std::string_view kDriverName = "nvidia";
inline constexpr std::string_view kNvdecToken = "nvdec";

struct Quirks {
    bool modifiers_supported = false;
    bool force_linear = true;
};

inline constexpr const char* name() noexcept { return "nvidia"; }

[[nodiscard]] bool matches(const std::string& driver_name) noexcept;

[[nodiscard]] const Quirks& quirks() noexcept;

}
