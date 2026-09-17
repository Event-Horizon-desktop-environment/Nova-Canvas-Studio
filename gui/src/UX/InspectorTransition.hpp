#pragma once

class QToolButton;
class QVBoxLayout;

namespace canvas::gui {
class MainWindow;
class TimelineWidget;

void build_inspector_transition(MainWindow& main_window, QVBoxLayout* transition_layout,
                                QToolButton* transition_mode_btn);
void attach_inspector_transition(MainWindow& main_window, TimelineWidget* timeline);
void update_inspector_transition(MainWindow& main_window);
void apply_inspector_transition(MainWindow& main_window);
}
