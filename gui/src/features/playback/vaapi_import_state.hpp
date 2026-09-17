#pragma once

namespace canvas::gui {

[[nodiscard]] bool vaapi_viewer_import_available() noexcept;

void set_vaapi_viewer_import_available(bool available) noexcept;

}
