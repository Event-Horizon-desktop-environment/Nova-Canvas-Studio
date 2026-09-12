#pragma once

// Active appearance mode (light / dark / hypr-dark) and the apply machinery
// built on it: switching modes re-applies the QPalette + global stylesheet and
// runs every registered re-apply callback, plus the per-widget helpers
// (apply_theme_style and apply_panel_shadow) that register those callbacks.
// "hypr-dark" is the Dark palette pre-lifted to cancel Hyprland's native-
// Wayland sRGB quantization (see theme_tokens.cpp); it auto-activates only
// when apply_theme() saw the Hyprland Wayland backend, so normal dark/light
// toggling still lands in the uncompensated family.

#include <QString>

#include <functional>

class QApplication;
class QWidget;

namespace canvas::gui {

bool is_light();

// True while the Hyprland-compensated dark set is active (auto-set by main()
// on Hyprland's native Wayland backend; not user-selectable in the menu).
bool is_hypr_dark();

// Flips the active appearance mode. Re-applies the application palette and
// global stylesheet and runs all registered re-apply callbacks (per-widget
// style builders). No-op when `light` equals the current mode. HyprDark state
// is untouched, so flipping back to dark returns to the compensated set.
void set_light(bool light);

// Toggles the Hyprland compensation layer on the dark family. Re-applies the
// palette/stylesheet/re-apply callbacks exactly like set_light(). No-op when
// `enabled` equals the current HyprDark state.
void set_hypr_dark(bool enabled);

// Registers `fn` to be invoked after every single-light flip (see set_light).
void register_theme_reapply(std::function<void()> fn);

// Applies a themed stylesheet to `w` now, and re-applies it whenever the
// appearance mode switches (see set_light). `style` is invoked each time, so it
// must read tokens() at call time rather than cache colors.
void apply_theme_style(QWidget* w, const std::function<QString()>& style);

// Soft elevation shadow behind a panel widget (docks, media pool cards).
void apply_panel_shadow(QWidget* w);

// Applies the theme to the application: installs HorizonStyle over Fusion,
// builds the QPalette from the active token set, sets the global flat-controls
// stylesheet, and installs popup rounding. Call once from main() before
// showing windows. App-specific chrome is additionally styled via the
// per-widget stylesheet helpers in UX/theme_styles.hpp, applied to individual
// widgets.
void apply_theme(QApplication& app, bool light = false, bool hypr_dark = false);

}  // namespace canvas::gui