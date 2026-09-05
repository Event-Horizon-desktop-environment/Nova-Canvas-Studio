#pragma once

#include <QWidget>

#include "canvas/core/export/render_queue.hpp"

class QListWidget;
class QPushButton;
class QProgressBar;
class QLabel;

namespace canvas::gui {

// Right-hand render queue panel for the Deliver page. Displays queued/running/
// finished jobs from an canvas::core::RenderQueue and refreshes on change.
class RenderQueuePanel : public QWidget {
    Q_OBJECT

public:
    explicit RenderQueuePanel(QWidget* parent = nullptr);

    // Reflect the given queue into the list.
    void set_queue(canvas::core::RenderQueue* queue);

    // Called whenever the queue changes; reads jobs() and repaints.
    void refresh();

signals:
    void render_all_clicked();
    void cancel_all_clicked();
    void clear_finished_clicked();
    void job_cancel_clicked(uint64_t id);

private:
    void build();

    canvas::core::RenderQueue* queue_ = nullptr;
    QListWidget* list_ = nullptr;
    QProgressBar* overall_ = nullptr;
    QLabel* summary_ = nullptr;
    QPushButton* render_all_ = nullptr;
};

}  // namespace canvas::gui
