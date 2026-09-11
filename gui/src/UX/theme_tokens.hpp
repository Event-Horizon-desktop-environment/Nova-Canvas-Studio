#pragma once

// Theme design tokens: the per-mode color/font set, the active-token lookup
// (tokens()), and the color → QSS helpers that every style builder and
// paint-code widget shares. Tuning a palette (surfaces, ink, accent, clip
// stripe colors) lives here and here only.

#include <QColor>
#include <QString>

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
    QColor border_hi;      // == border (kept for wire-compat; no specular catch-light)
    QColor ink;            // primary text
    QColor ink_muted;      // secondary text
    QColor ink_faint;      // tertiary text
    QColor accent;         // brand accent (Nova gold)
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
    QColor clip_border_video;  // resting outline, video clips (cool)
    QColor clip_border_audio;  // resting outline, audio clips (green-cast)
    QColor clip_shadow;    // timeline clip baseline shadow
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

// Unified spacing grid + typographic scale shared by every surface. The same
// constants feed the QSS builders (via literals pulled from here) AND the C++
// layout code, so spacing/size can never drift between stylesheet and code.
// Grid: 4px base -> 4/8/12/16/24. Type: 12/14/16/20 (QSS px at logical res).
namespace layout {
inline constexpr int kSpace4 = 4;    // tight icon-to-text gaps, inner chips
inline constexpr int kSpace8 = 8;    // default spacing between siblings
inline constexpr int kSpace12 = 12;  // group separation / well gutters
inline constexpr int kSpace16 = 16;  // panel padding / card insets
inline constexpr int kSpace24 = 24;  // section separation / hero padding

inline constexpr int kTypeCaption = 12;  // labels, metadata, hints
inline constexpr int kTypeBody = 14;     // default body / interactive text
inline constexpr int kTypeTitle = 16;    // section titles, panel headers
inline constexpr int kTypeDisplay = 20;  // hero numbers, empty-state titles
}  // namespace layout

// Active token set for the current appearance mode. Read at use time — style
// lambdas must call this rather than capture colors, so set_light() re-applies
// cleanly.
const ThemeTokens& tokens();

// Always-on startup dump of every active color token to the log (hex, incl.
// alpha) so a running binary's palette can be diffed without digging in source.
void log_theme_tokens();

// QSS-safe colour string (#rrggbb, or rgba(r,g,b,a) when translucent).
QString css(const QColor& c);

// Returns `c` with its alpha overridden. Convenience for glow / state colors
// whose hue is mode-dependent but whose opacity is fixed.
QColor with_alpha(const QColor& c, int alpha);

}  // namespace canvas::gui