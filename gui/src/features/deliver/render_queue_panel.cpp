#include "features/deliver/render_queue_panel.hpp"

#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QListView>
#include <QProgressBar>
#include <QPushButton>
#include <QVBoxLayout>

#include <cmath>
#include <string>

namespace canvas::gui {

namespace {

QString status_text(canvas::core::RenderJob::Status s) {
    using S = canvas::core::RenderJob::Status;
    switch (s) {
        case S::Queued: return QStringLiteral("Queued");
        case S::Rendering: return QStringLiteral("Rendering");
        case S::Completed: return QStringLiteral("Completed");
        case S::Failed: return QStringLiteral("Failed");
        case S::Cancelled: return QStringLiteral("Cancelled");
    }
    return QStringLiteral("Queued");
}

}  // namespace

RenderQueuePanel::RenderQueuePanel(QWidget* parent) : QWidget(parent) {
    build();
    refresh();
}

void RenderQueuePanel::build() {
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(10, 10, 10, 10);
    root->setSpacing(8);

    auto* title = new QLabel(tr("Render Queue"));
    title->setStyleSheet(QStringLiteral("color:#E8EAF0;font-size:13px;font-weight:600;"));
    root->addWidget(title);

    summary_ = new QLabel(tr("No jobs in queue."));
    summary_->setStyleSheet(QStringLiteral("color:#9AA0B0;font-size:11px;"));
    root->addWidget(summary_);

    overall_ = new QProgressBar(this);
    overall_->setRange(0, 1000);
    overall_->setValue(0);
    overall_->setTextVisible(true);
    overall_->setStyleSheet(QStringLiteral(
        "QProgressBar{background:#0C0E14;border:1px solid #232833;border-radius:4px;color:#C6CAD6;}"
        "QProgressBar::chunk{background:#2E6FD8;border-radius:3px;}"));
    root->addWidget(overall_);

    list_ = new QListWidget(this);
    list_->setStyleSheet(QStringLiteral(
        "QListWidget{background:#0C0E14;border:1px solid #232833;border-radius:4px;color:#E8EAF0;}"
        "QListWidget::item{height:52px;border-bottom:1px solid #1A1D27;}"));
    list_->setSelectionMode(QAbstractItemView::SingleSelection);
    root->addWidget(list_, 1);

    auto* buttons = new QHBoxLayout;
    render_all_ = new QPushButton(tr("Render"));
    render_all_->setStyleSheet(QStringLiteral("QPushButton{background:#2E6FD8;color:white;"
                                              "border-radius:5px;padding:6px 12px;}"));
    auto* cancel_all = new QPushButton(tr("Cancel All"));
    cancel_all->setStyleSheet(QStringLiteral("QPushButton{background:#3A2C2C;color:#E8EAF0;"
                                             "border-radius:5px;padding:6px 12px;}"));
    auto* clear = new QPushButton(tr("Clear Finished"));
    clear->setStyleSheet(QStringLiteral("QPushButton{background:#1A1D27;color:#C6CAD6;"
                                        "border-radius:5px;padding:6px 12px;}"));
    buttons->addWidget(render_all_);
    buttons->addWidget(cancel_all);
    buttons->addWidget(clear);
    root->addLayout(buttons);

    connect(render_all_, &QPushButton::clicked, this, &RenderQueuePanel::render_all_clicked);
    connect(cancel_all, &QPushButton::clicked, this, &RenderQueuePanel::cancel_all_clicked);
    connect(clear, &QPushButton::clicked, this, &RenderQueuePanel::clear_finished_clicked);
}

void RenderQueuePanel::set_queue(canvas::core::RenderQueue* queue) {
    queue_ = queue;
    refresh();
}

void RenderQueuePanel::refresh() {
    if (!queue_) return;
    const auto jobs = queue_->jobs();

    list_->clear();
    int running = 0, done = 0, queued = 0;
    double total_progress = 0.0;
    for (const auto& j : jobs) {
        using S = canvas::core::RenderJob::Status;
        QString line = QString::fromStdString(j.name);
        if (!j.output_path.empty())
            line += QStringLiteral("\n  %1").arg(QString::fromStdString(j.output_path));
        line += QStringLiteral("  [%1]").arg(status_text(j.status));
        if (j.status == S::Rendering && j.render_fps > 0.0)
            line += QStringLiteral("  %1 fps").arg(j.render_fps, 0, 'f', 1);
        if (!j.error.empty())
            line += QStringLiteral("  %1").arg(QString::fromStdString(j.error));
        list_->addItem(line);

        if (j.status == S::Queued) ++queued;
        else if (j.status == S::Rendering) ++running;
        else ++done;
        if (j.status == S::Rendering) total_progress += j.progress;
        else if (j.status == S::Completed) total_progress += 1.0;
    }

    const int total = (int)jobs.size();
    if (total > 0) {
        summary_->setText(tr("%1 jobs — %2 queued, %3 running, %4 done")
                              .arg(total).arg(queued).arg(running).arg(done));
        overall_->setValue((int)std::lround(total_progress / total * 1000.0));
        // "Render" is the manual start trigger: enable it whenever there is
        // staged (Queued) work and nothing is actively Rendering, so the user can
        // kick off the batch. is_busy() would be wrong here — it also reports
        // true for Queued-but-not-started jobs, which would disable the button
        // exactly when it is needed.
        render_all_->setEnabled(running == 0 && queued > 0);
    } else {
        summary_->setText(tr("No jobs in queue."));
        overall_->setValue(0);
        render_all_->setEnabled(false);
    }
}

}  // namespace canvas::gui
