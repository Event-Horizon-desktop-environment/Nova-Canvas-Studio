#pragma once

#include <QColor>
#include <QIcon>
#include <QString>

#include <functional>

class QApplication;
class QMenu;
class QWidget;

namespace canvas::gui {

// Design tokens for one appearance mode. Used both for the QPalette and, via
// css(), by the QSS builders and paint-code widgets, so a surface can never
// drift between palette and stylesheet on a mode switch.
struct ThemeTokens {
    QColor surface;        // window / deepest base
    QColor surface_low;    // recessed wells
    QColor surface_raised; // panels, cards, buttons
    QColor surface_higher; // hovered raised surfaces
    QColor surface_highest;// menus / popups
    QColor border;         // strong separators
    QColor border_soft;    // hairlines
    QColor border_hi;      // lit top-edge highlight (glass catch-light)
    QColor ink;            // primary text
    QColor ink_muted;      // secondary text
    QColor ink_faint;      // tertiary text
    QColor accent;         // brand accent (mint)
    QColor accent_hover;
    QColor accent_press;
    QColor on_accent;      // text on solid accent fills
    QColor accent_text;    // accent-tinted text on surfaces
    QColor accent_soft;    // translucent accent fill (state/selection)
    QColor accent_line;    // translucent accent border (checked)
    QColor playhead;       // timeline playhead
    QColor playhead_soft;  // translucent playhead fill
    QColor clip_video;     // timeline video-clip well
    QColor clip_audio;     // timeline audio-clip well
    QColor clip_label;     // timeline clip title bar
    QColor danger;
    QColor danger_soft;
    QColor warn;
    QColor state_hover;    // translucent ink overlays (M3 state layers)
    QColor state_press;
    QColor state_selected;
    QColor focus_ring;
    QString font_ui;
    QString font_mono;
};

// Active appearance + registered re-apply callbacks.
bool is_light();
void set_light(bool light);
void register_theme_reapply(std::function<void()> fn);

// Applies a themed stylesheet to `w` now, and re-applies it whenever the
// appearance mode switches (see set_light). `style` is invoked each time, so it
// must read tokens() at call time rather than cache colors.
void apply_theme_style(QWidget* w, const std::function<QString()>& style);

// Soft elevation shadow behind a panel widget (docks, media pool cards).
void apply_panel_shadow(QWidget* w);

const ThemeTokens& tokens();

// Resolve-style clip colour swatches (12 entries, indexed 0-11 = colour 1-12).
// Shared by the File-inspector picker and the timeline clip stripe so the two
// never drift.
const QColor* clip_color_swatches();
QColor clip_color_for(uint8_t color); // 0 -> invalid QColor (no colour)

// QSS-safe colour string (#rrggbb, or rgba(r,g,b,a) when translucent).
QString css(const QColor& c);

// Returns `c` with its alpha overridden. Convenience for glow / state colors
// whose hue is mode-dependent but whose opacity is fixed.
QColor with_alpha(const QColor& c, int alpha);

// Applies the Material 3 dark design system: a QPalette built from M3 color
// roles plus a comprehensive application-level stylesheet (buttons use
// filled/tonal/outlined styles with full-corner pill shapes, state-layer
// overlays, surface-container layering, etc.). Call once from main() before
// showing windows. `light` selects the light token set. App-specific chrome is
// additionally styled via the per-widget stylesheet helpers below, applied to
// individual widgets.
void apply_theme(QApplication& app, bool light = false);

// Dropdown / popup card rounding. A QMenu is a native popup window: a plain
// QSS border-radius leaves the four corners square (measured). These helpers
// make the popup a translucent, frameless window so the theme's rounded card
// background actually clips — call apply_rounded_menu before the menu is shown.
QMenu* make_rounded_menu(QWidget* parent);
void apply_rounded_menu(QMenu* menu);

// Installs a qApp-wide event filter that applies the same translucent-popup
// rounding to popups not created through make_rounded_menu (e.g. QComboBox
// dropdown containers, QMenuBar-owned submenus). Idempotent; called from
// apply_theme.
void install_popup_rounding(QApplication& app);

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

QString transport_bar_style();
QString timeline_tools_style();
QString page_switcher_style();
QString time_label_style();
QString dock_glow_style();
QString dock_panel_style();
QString left_tab_strip_style();
QString media_pool_style();
QString viewer_frame_style();
QString timeline_frame_style();
QString global_toolbar_style();
QString top_status_bar_style();
QString big_timecode_style();
QString bin_tree_style();
QString inspector_category_header_style();
QString inspector_card_header_open_style();
QString inspector_card_header_closed_style();
QString inspector_card_body_style();
QString inspector_tab_track_style();
QString inspector_tab_style();
QString page_pill_style();
QString flat_tool_style();
QString tool_cluster_style();
QString outline_pill_style();
QString slider_style();

}  // namespace canvas::gui