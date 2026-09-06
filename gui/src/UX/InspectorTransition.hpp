#pragma once

// Transition inspector page (splitplan refactor): Start/End sub-tab pills plus
// Video + Audio property categories shown when a transition bubble is selected
// on the timeline. Edits commit through the normal edit_ops path so every
// change is undoable.

class QVBoxLayout;

namespace canvas::gui {
class MainWindow;
class TimelineWidget;

void build_inspector_transition(MainWindow& main_window, QVBoxLayout* transition_layout);
void attach_inspector_transition(MainWindow& main_window, TimelineWidget* timeline);
void update_inspector_transition(MainWindow& main_window);
void apply_inspector_transition(MainWindow& main_window);
}  // namespace canvas::gui