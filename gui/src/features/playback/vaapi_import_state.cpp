#include "features/playback/vaapi_import_state.hpp"

namespace canvas::gui {

namespace {

bool g_vaapi_viewer_import_available = false;

}

bool vaapi_viewer_import_available() noexcept {
    return g_vaapi_viewer_import_available;
}

void set_vaapi_viewer_import_available(const bool available) noexcept {
    g_vaapi_viewer_import_available = available;
}

}
