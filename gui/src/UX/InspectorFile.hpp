#pragma once

// File inspector page (splitplan refactor): read-only source header info plus
// fully-wired metadata (tag, colour, name, notes) that commits through
// edit_ops, an audio-channel configuration section, and timecode readouts.
// Construction and wiring live in InspectorFile.cpp.

class QVBoxLayout;

namespace canvas::gui {
class MainWindow;
class TimelineWidget;

void build_inspector_file(MainWindow& main_window, QVBoxLayout* file_layout);
void attach_inspector_file(MainWindow& main_window, TimelineWidget* timeline);
void update_inspector_file(MainWindow& main_window);
void apply_inspector_file(MainWindow& main_window);
}  // namespace canvas::gui