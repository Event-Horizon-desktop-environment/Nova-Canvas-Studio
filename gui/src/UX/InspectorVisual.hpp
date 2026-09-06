#pragma once

// The Inspector's Transform + Composite categories, driven by the selected
// clip's shared visual fields (splitplan refactor). The whole video tab's
// category block builds through InspectorVisual.cpp, owns its control widgets,
// refreshes from the model whenever the timeline selection changes, and commits
// any edit as one undoable command. Declared here and friended in MainWindow so
// the implementation can read the private selection/project/undo state.

class QVBoxLayout;
class TimelineWidget;

namespace canvas::gui {

class MainWindow;

// Builds the Video tab's property categories (Transform, Cropping, Dynamic
// Zoom, Composite, Speed Change, Stabilization, Lens Correction, Retime and
// Scaling) into `video_layout` and stores the control handles for refresh/apply.
void build_inspector_visual(MainWindow& mw, QVBoxLayout* video_layout);

// Connects the built controls to the timeline's selection signals so the
// categories refresh whenever a clip is (de)selected. Call AFTER connect_timeline()
// (timeline_ exists then); selection lambdas already updated selected_clip_.
void attach_inspector_visual(MainWindow& mw, TimelineWidget* timeline);

// Refreshes the Transform/Composite widgets from the selected clip. No-op (keeps
// current widget values) when nothing is selected.
void update_inspector_visual(MainWindow& mw);

// VisualPart bitmask selecting which groups to commit.
enum VisualPart {
    VisualPartNone = 0,
    VisualPartTransform = 1 << 0,
    VisualPartComposite = 1 << 1,
    VisualPartAll = VisualPartTransform | VisualPartComposite,
};

// Commits `parts` of the visible control values for the selected clip as one
// undoable edit each. Undo/redo resync is the caller's job via
// update_inspector_visual().
void apply_inspector_visual(MainWindow& mw, unsigned parts);
void apply_inspector_visual(MainWindow& mw);  // both parts

}  // namespace canvas::gui