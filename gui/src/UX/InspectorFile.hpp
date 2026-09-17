#pragma once

class QVBoxLayout;

namespace canvas::gui {
class MainWindow;
class TimelineWidget;

void build_inspector_file(MainWindow& main_window, QVBoxLayout* file_layout);
void attach_inspector_file(MainWindow& main_window, TimelineWidget* timeline);
void update_inspector_file(MainWindow& main_window);
void apply_inspector_file(MainWindow& main_window);
}
