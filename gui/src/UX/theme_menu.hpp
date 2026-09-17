#pragma once

class QApplication;
class QMenu;
class QWidget;

namespace canvas::gui {

QMenu* make_rounded_menu(QWidget* parent);

void apply_rounded_menu(QMenu* menu);

void install_popup_rounding(QApplication& app);

}
