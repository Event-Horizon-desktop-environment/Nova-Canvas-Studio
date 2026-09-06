#pragma once

// The Inspector's Audio tab property categories: Volume/Pan (existing), Pitch,
// Speed Change, Equalizer, and the AI placeholder sections. Every wired control
// reads the selected audio clip's fields, and any edit commits as one undoable
// audio-processing op (set_clip_audio_processing). The tab's controls are only
// usable while an Audio-track clip is selected.
//
// Declared here and friended in MainWindow so the implementation can touch the
// private selection/project/undo state (same splitplan pattern as
// InspectorVisual).

class QVBoxLayout;
class QToolButton;
class TimelineWidget;

namespace canvas::gui {

class MainWindow;

// Builds the Audio tab's property categories into `audio_layout` and stores the
// control handles for refresh/apply. `audio_mode_button` is the inspector's
// Audio pill button (its enabled state tracks whether an audio clip is
// selected).
void build_inspector_audio(MainWindow& mw, QVBoxLayout* audio_layout,
                           QToolButton* audio_mode_button);

// Connects the built controls to the timeline's selection signals so the tab
// refreshes/enables whenever a clip is (de)selected. Call AFTER
// connect_timeline().
void attach_inspector_audio(MainWindow& mw, TimelineWidget* timeline);

// Refreshes the Audio tab's widgets from the selected clip and enables the tab
// only while an Audio-track clip is selected. Replaces the older
// MainWindow::update_inspector_audio (Volume/Pan) — call this instead.
void update_inspector_audio_full(MainWindow& mw);

// Commits the wired audio-processing controls (pitch/speed/EQ) for the selected
// clip as one undoable edit.
void apply_inspector_audio_processing(MainWindow& mw);

}  // namespace canvas::gui