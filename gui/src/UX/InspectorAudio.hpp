#pragma once

class QVBoxLayout;
class QToolButton;
class TimelineWidget;

namespace canvas::gui {

class MainWindow;

void build_inspector_audio(MainWindow& mw, QVBoxLayout* audio_layout,
                           QToolButton* audio_mode_button);

void attach_inspector_audio(MainWindow& mw, TimelineWidget* timeline);

void update_inspector_audio_full(MainWindow& mw);

void apply_inspector_audio_processing(MainWindow& mw);

}
