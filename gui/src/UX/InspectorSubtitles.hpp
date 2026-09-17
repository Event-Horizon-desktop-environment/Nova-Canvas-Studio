#pragma once

class QVBoxLayout;

namespace canvas::gui {
class MainWindow;
class TimelineWidget;

void build_inspector_subtitles(MainWindow& main_window, QVBoxLayout* subtitles_layout);
void attach_inspector_subtitles(MainWindow& main_window, TimelineWidget* timeline);
void update_inspector_subtitles(MainWindow& main_window);
void apply_inspector_subtitles(MainWindow& main_window);
}
