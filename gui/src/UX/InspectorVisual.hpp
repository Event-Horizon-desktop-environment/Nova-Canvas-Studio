#pragma once

class QVBoxLayout;
class TimelineWidget;

namespace canvas::gui {

class MainWindow;

void build_inspector_visual(MainWindow& mw, QVBoxLayout* video_layout);

void attach_inspector_visual(MainWindow& mw, TimelineWidget* timeline);

void update_inspector_visual(MainWindow& mw);

enum VisualPart {
    VisualPartNone = 0,
    VisualPartTransform = 1 << 0,
    VisualPartComposite = 1 << 1,
    VisualPartTitle = 1 << 2,
    VisualPartAll = VisualPartTransform | VisualPartComposite | VisualPartTitle,
};

void apply_inspector_visual(MainWindow& mw, unsigned parts);
void apply_inspector_visual(MainWindow& mw);

}
