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

class RenderQueuePanel : public QWidget {
    Q_OBJECT

public:
    struct JobRow {
        QListWidgetItem* item = nullptr;
        QLabel* status = nullptr;
        QLabel* primary = nullptr;
        QLabel* path = nullptr;
    };

    explicit RenderQueuePanel(QWidget* parent = nullptr);

    void set_queue(canvas::core::RenderQueue* queue);

    void set_project_name(const QString& name);

    void refresh();

signals:
    void render_all_clicked();
    void cancel_all_clicked();
    void clear_queued_clicked();
    void job_remove_clicked(uint64_t id);
    void job_edit_clicked(uint64_t id);

private:
    void build();

    canvas::core::RenderQueue* queue_ = nullptr;
    QString project_name_;
    QListWidget* list_ = nullptr;
    QProgressBar* overall_ = nullptr;
    QLabel* overall_pct_ = nullptr;
    QLabel* summary_ = nullptr;
    QPushButton* render_all_ = nullptr;
    QPushButton* pause_ = nullptr;
    QHash<uint64_t, JobRow> rows_;
};

}
