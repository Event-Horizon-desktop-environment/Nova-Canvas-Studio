#pragma once

#include "canvas/core/media/vaapi/surface.hpp"

#include <string>
#include <string_view>

namespace canvas::core::vaapi::intel {

inline constexpr std::string_view kIhdDriverName = "iHD";
inline constexpr std::string_view kI965DriverName = "i965";

struct Quirks {
    bool modifiers_supported = true;
    bool modifiers_retry_linear = true;
};

inline constexpr const char* name() noexcept { return "intel"; }

[[nodiscard]] bool matches(const std::string& driver_name) noexcept;
[[nodiscard]] bool is_i965(const std::string& driver_name) noexcept;

[[nodiscard]] const Quirks& quirks() noexcept;

}
