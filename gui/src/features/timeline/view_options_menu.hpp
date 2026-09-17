#pragma once

#include <QToolButton>

namespace canvas::gui {

class MainWindow;

void attach_timeline_view_options_button(MainWindow& mw, QToolButton* button);

void apply_view_options(MainWindow& mw);

}
