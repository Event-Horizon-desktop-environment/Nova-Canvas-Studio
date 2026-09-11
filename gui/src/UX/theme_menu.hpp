#pragma once

// Rounded popup/menu cards. A QMenu is a native popup window: a plain QSS
// border-radius leaves the four corners square. These helpers make popups
// translucent + frameless so the theme's rounded card background actually
// clips, either explicitly (make_rounded_menu / apply_rounded_menu) or via a
// qApp-wide event filter for popups created elsewhere (QComboBox dropdowns,
// menubar submenus).

class QApplication;
class QMenu;
class QWidget;

namespace canvas::gui {

// Creates a themed rounded menu card.
QMenu* make_rounded_menu(QWidget* parent);

// Applies the rounded-card treatment to a menu already created elsewhere —
// call before the menu is shown.
void apply_rounded_menu(QMenu* menu);

// Installs a qApp-wide event filter that applies the same translucent-popup
// rounding to popups not created through make_rounded_menu (e.g. QComboBox
// dropdown containers, QMenuBar-owned submenus). Idempotent; called from
// apply_theme.
void install_popup_rounding(QApplication& app);

}  // namespace canvas::gui