#pragma once

// Bundled SVG icon loading with theme tinting. icon() renders a monochrome
// resource through the theme ink (Disabled/Active/Selected per QIcon mode for
// nice hover/disabled feedback); raw_icon() preserves the authored colors for
// full-colour artwork; icon(name, color) pins the idle tint to a fixed color
// for accent-coded badges. Rendering/tinting lives in theme_icons.cpp.

#include <QColor>
#include <QIcon>

namespace canvas::gui {

// Loads a bundled SVG icon by short name, e.g. icon("blade") maps to the
// ":/icons/blade.svg" resource. Rendered via QSvgRenderer at the requested
// size with per-mode tinting (Disabled/Active/Selected), so tool-button icons
// stay crisp and readable against the active theme.
QIcon icon(const char* name);

// Loads a bundled SVG icon WITHOUT any theme tinting, preserving the source
// colours. Use this for full-colour artwork such as the app icon.
QIcon raw_icon(const char* name);

// As above but fixes the idle (Normal/Selected) tint to a specific color, for
// accent-coded icons (e.g. marker / track-color swatch badges).
QIcon icon(const char* name, const QColor& normal);

}  // namespace canvas::gui