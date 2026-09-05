#pragma once

#include <QColor>
#include <QIcon>
#include <QString>

class QApplication;

namespace canvas::gui {

// Applies the Material 3 dark design system: a QPalette built from M3 color
// roles plus a comprehensive application-level stylesheet (buttons use
// filled/tonal/outlined styles with full-corner pill shapes, state-layer
// overlays, surface-container layering, etc.). Call once from main() before
// showing windows. App-specific chrome is additionally styled via the
// per-widget stylesheet helpers below, applied to individual widgets.
void apply_theme(QApplication& app);

// Loads a bundled SVG icon by short name, e.g. icon("blade") maps to the
// ":/icons/blade.svg" resource. Rendered via QSvgRenderer at the requested
// size with per-mode tinting (Disabled/Active/Selected), so tool-button icons
// stay crisp and readable against the dark theme.
QIcon icon(const char* name);

// Loads a bundled SVG icon WITHOUT any theme tinting, preserving the source
// colours. Use this for full-colour artwork such as the app icon.
QIcon raw_icon(const char* name);

// As above but fixes the idle (Normal/Selected) tint to a specific color, for
// accent-coded icons (e.g. marker / track-color swatch badges).
QIcon icon(const char* name, const QColor& normal);

QString transport_bar_style();
QString timeline_tools_style();
QString page_switcher_style();
QString time_label_style();
QString media_pool_style();
QString viewer_frame_style();
QString timeline_frame_style();
QString global_toolbar_style();
QString top_status_bar_style();
QString big_timecode_style();
QString bin_tree_style();
QString inspector_category_header_style();
QString inspector_body_style();
QString page_pill_style();
QString flat_tool_style();
QString tool_cluster_style();
QString outline_pill_style();
QString slider_style();

}  // namespace canvas::gui
