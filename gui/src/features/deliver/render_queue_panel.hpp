#pragma once

#include <QHash>
#include <QWidget>

#include "canvas/core/export/render_queue.hpp"

class QListWidget;
class QListWidgetItem;
class QPushButton;
class QProgressBar;
class QLabel;

namespace canvas::gui {

// Right-hand render queue panel for the Deliver page. Displays queued/running/
// finished jobs from an canvas::core::RenderQueue and refreshes on change.
class RenderQueuePanel : public QWidget {
    Q_OBJECT

public:
    // Cached per-job row widgets so progress ticks only relabel existing
    // controls instead of recreating the whole list.
    struct JobRow {
        QListWidgetItem* item = nullptr;
        QLabel* status = nullptr;
        QLabel* primary = nullptr;
        QLabel* path = nullptr;
    };

    explicit RenderQueuePanel(QWidget* parent = nullptr);

    // Reflect the given queue into the list.
    void set_queue(canvas::core::RenderQueue* queue);

    // Called whenever the queue changes; reads jobs() and repaints. Updates
    // existing job rows in place (no flicker during progress ticks).
    void refresh();

signals:
    void render_all_clicked();
    void cancel_all_clicked();
    void clear_queued_clicked();
    // Close/X hit on a job row: removes the queued/finished job, or cancels a
    // rendering one.
    void job_remove_clicked(uint64_t id);
    // Pencil hit on a job row (scaffold — nothing wired to edit yet).
    void job_edit_clicked(uint64_t id);

private:
    void build();

    canvas::core::RenderQueue* queue_ = nullptr;
    QListWidget* list_ = nullptr;
    QProgressBar* overall_ = nullptr;
    QLabel* overall_pct_ = nullptr;
    QLabel* summary_ = nullptr;
    QPushButton* render_all_ = nullptr;
    QHash<uint64_t, JobRow> rows_;
};

}  // namespace canvas::gui
