// Qt-free storage for the VAAPI EGL-import capability flag (see the header).

#include "features/playback/vaapi_import_state.hpp"

namespace canvas::gui {

namespace {

// Default false: until ViewerGL has probed its EGL session nothing on the
// decode side may assume zero-copy VAAPI frames can be displayed. Once the
// viewer publishes `true` it stays true for the process lifetime — no live
// EGL session loses the extension.
bool g_vaapi_viewer_import_available = false;

}  // namespace

bool vaapi_viewer_import_available() noexcept {
    return g_vaapi_viewer_import_available;
}

void set_vaapi_viewer_import_available(const bool available) noexcept {
    g_vaapi_viewer_import_available = available;
}

}  // namespace canvas::gui