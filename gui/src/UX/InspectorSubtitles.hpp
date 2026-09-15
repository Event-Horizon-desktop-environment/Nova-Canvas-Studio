#pragma once

// Subtitles inspector page: caption styling for the selected title/caption
// clip(s). Size slider (percent of frame height), Zoom Out/In buttons (x0.8 /
// x1.25 one-shot commits), and a font dropdown fed by the Qt-free
// title::installed_font_families() catalogue (system fonts, "System Default" =
// the default face). Shadow and Box effect categories add a blurred/tinted
// drop shadow and a background box — each with offset/padding/radius/opacity
// sliders and a colour picker button (opens QColorDialog). The sliders are
// REALTIME: dragging streams the change through a warm swap_project snapshot so
// the current frame re-presents live, and exactly one undo entry is recorded on
// release. "Select All Subtitles" gathers every caption into one selection;
// with several selected, every edit applies to all of them as ONE undoable
// GroupCommand. Every commit goes through set_clip_title/set_clip_transform —
// clamped and undoable — and re-syncs the Video tab's Title controls so the two
// pages never disagree.

class QVBoxLayout;

namespace canvas::gui {
class MainWindow;
class TimelineWidget;

void build_inspector_subtitles(MainWindow& main_window, QVBoxLayout* subtitles_layout);
void attach_inspector_subtitles(MainWindow& main_window, TimelineWidget* timeline);
void update_inspector_subtitles(MainWindow& main_window);
void apply_inspector_subtitles(MainWindow& main_window);
}  // namespace canvas::gui