#pragma once

// Timeline view-options dropdown (transport bar, Resolve's Timeline > View
// Options): a popup menu of view toggles, radio submenus, waveform-detail
// flags, a Fixed Playhead toggle, Track Height sliders and a "Set as Default
// View" persistence action. Full option surface lives in the pure-data
// TimelineViewOptions struct (grief-free: no Qt). MainWindow owns the struct,
// the menu reads/writes it, apply_view_options() walks it into the timeline
// and viewer, and save/load ride QSettings.

#include <QToolButton>

namespace canvas::gui {

class MainWindow;

// Builds the dropdown menu, attaches it to `button` (InstantPopup + themed
// style is the caller's job), loads any persisted "Set as Default View"
// baseline into MainWindow's options, and wires every action. Edits apply
// immediately and stay session-scoped until "Set as Default View" persists.
void attach_timeline_view_options_button(MainWindow& mw, QToolButton* button);

// Reflects MainWindow's current TimelineViewOptions into the timeline
// (view-options pointer, fixed-playhead, full scene rebuild under the new
// flags) and the viewer (canvas background color/checker).
void apply_view_options(MainWindow& mw);

}  // namespace canvas::gui