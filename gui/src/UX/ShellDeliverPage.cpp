#include "UX/MainWindow.hpp"

#include "features/deliver/deliver_settings_panel.hpp"
#include "features/deliver/render_queue_panel.hpp"

#include <QDockWidget>
#include <QMessageBox>
#include <QMetaObject>

#include <string>

namespace canvas::gui {

// The Deliver page: settings panel + render queue, docked left/right, with all
// the render-queue signal plumbing and error dialogs. Extracted from
// ShellCenter.cpp (splitplan refactor) so the center workspace builds the
// viewer/timeline without dragging the deliver chrome along; the page is
// hidden by default and re-shown when the page bar switches to Deliver.
void build_deliver_docks(MainWindow& mw) {
    // ---- 7b. DELIVER page docks — settings left, render queue right ----
    mw.deliver_settings_ = new DeliverSettingsPanel(&mw);
    mw.deliver_settings_->setObjectName(QStringLiteral("deliverSettings"));
    mw.deliver_settings_dock_ = new QDockWidget(MainWindow::tr("Deliver Settings"), &mw);
    mw.deliver_settings_dock_->setObjectName(QStringLiteral("deliverSettingsDock"));
    mw.deliver_settings_dock_->setWidget(mw.deliver_settings_);
    mw.deliver_settings_dock_->setFeatures(QDockWidget::DockWidgetMovable | QDockWidget::DockWidgetFloatable);
    mw.deliver_settings_dock_->setMinimumWidth(360);
    mw.addDockWidget(Qt::LeftDockWidgetArea, mw.deliver_settings_dock_);
    mw.deliver_settings_dock_->hide();

    mw.deliver_queue_panel_ = new RenderQueuePanel(&mw);
    mw.deliver_queue_panel_->setObjectName(QStringLiteral("deliverQueue"));
    mw.deliver_queue_panel_->set_queue(&mw.render_queue_);
    mw.deliver_queue_dock_ = new QDockWidget(MainWindow::tr("Render Queue"), &mw);
    mw.deliver_queue_dock_->setObjectName(QStringLiteral("deliverQueueDock"));
    mw.deliver_queue_dock_->setWidget(mw.deliver_queue_panel_);
    mw.deliver_queue_dock_->setFeatures(QDockWidget::DockWidgetMovable | QDockWidget::DockWidgetFloatable);
    mw.deliver_queue_dock_->setMinimumWidth(300);
    mw.addDockWidget(Qt::RightDockWidgetArea, mw.deliver_queue_dock_);
    mw.deliver_queue_dock_->hide();

    // Deliver widget signals -> MainWindow actions.
    QObject::connect(mw.deliver_settings_, &DeliverSettingsPanel::add_to_queue_clicked, &mw,
            &MainWindow::add_current_to_render_queue);
    QObject::connect(mw.deliver_settings_, &DeliverSettingsPanel::render_all_clicked, &mw,
            &MainWindow::render_all_from_queue);
    QObject::connect(mw.deliver_queue_panel_, &RenderQueuePanel::render_all_clicked, &mw,
            &MainWindow::render_all_from_queue);
    QObject::connect(mw.deliver_queue_panel_, &RenderQueuePanel::clear_queued_clicked, &mw,
            [&mw] { mw.render_queue_.clear_queued(); mw.has_unsaved_changes_ = true;
                    mw.reflect_render_queue(); });
    QObject::connect(mw.deliver_queue_panel_, &RenderQueuePanel::job_remove_clicked, &mw,
            [&mw](uint64_t id) { mw.render_queue_.remove(id); mw.has_unsaved_changes_ = true;
                                 mw.reflect_render_queue(); });
    QObject::connect(mw.deliver_queue_panel_, &RenderQueuePanel::cancel_all_clicked, &mw,
            [&mw] { mw.render_queue_.cancel_all(); mw.has_unsaved_changes_ = true;
                    mw.reflect_render_queue(); });

    // Reflect queue changes into the UI panel (called on the main thread via a
    // queued-style refresh using QMetaObject to stay thread-safe with the
    // worker thread).
    mw.render_queue_.on_changed = [&mw] {
        QMetaObject::invokeMethod(&mw, [&mw] { mw.reflect_render_queue(); },
                                  Qt::QueuedConnection);
    };

    // Live export preview: the render worker pushes throttled composited export
    // frames; marshal them onto the GUI thread and present on the shared viewer
    // (the Deliver page's media preview) — exactly the pixels being encoded,
    // audio-free. The last preview frame stays on screen after the export ends.
    mw.render_queue_.on_preview_frame = [&mw](canvas::core::VideoFramePtr frame) {
        auto rf = std::make_shared<canvas::core::RenderFrame>();
        rf->a = std::move(frame);
        QMetaObject::invokeMethod(mw.viewer_,
                                  [&mw, rf = std::move(rf)] { mw.viewer_->set_frame(std::move(rf)); },
                                  Qt::QueuedConnection);
    };

    // While a job is being exported, park the thumbnail workers so their
    // full-res NVDEC decodes don't steal the render's GPU/disk bandwidth.
    // set_paused is atomic + CV-notified, so it can be driven straight from
    // the worker thread; requests queue up meanwhile and flush on finish.
    mw.render_queue_.on_job_started = [&mw](uint64_t) { mw.thumbnails_.set_paused(true); };

    // Pop a dialog when a render job fails, so the user isn't left guessing at
    // a bare "Failed" status. Errors are categorized so each kind of failure
    // gets its own popup type instead of a single generic message:
    //   - Configuration problems (missing output path / no codec selected) are
    //     shown as a persistent Information prompt to fix settings and retry.
    //   - Encoder/container/open-file failures are shown as a Warning with the
    //     FFmpeg detail.
    //   - Anything unexpected is shown as a Critical error.
    mw.render_queue_.on_job_finished = [&mw](uint64_t id) {
        mw.thumbnails_.set_paused(false);
        QMetaObject::invokeMethod(&mw, [&mw, id] {
            for (const auto& j : mw.render_queue_.jobs()) {
                if (j.id != id) continue;
                if (j.status != canvas::core::RenderJob::Status::Failed) break;
                const QString detail = j.error.empty()
                    ? MainWindow::tr("The renderer reported no error message. Check canvas_debug.log.")
                    : QString::fromStdString(j.error);
                const QString title = MainWindow::tr("Render Failed");
                if (j.error.find("Output path") != std::string::npos) {
                    QMessageBox::information(
                        &mw, title,
                        MainWindow::tr("There's no output location for \"%1\".\n\n%2\n\n"
                                       "Choose an output folder on the Deliver panel, then add to the queue again.")
                            .arg(QString::fromStdString(j.name), detail));
                } else if (j.error.find("codec") != std::string::npos ||
                           j.error.find("encoder") != std::string::npos) {
                    QMessageBox::warning(
                        &mw, title,
                        MainWindow::tr("\"%1\" couldn't start encoding.\n\n%2\n\n"
                                       "Try a different codec or encoder in Deliver settings.")
                            .arg(QString::fromStdString(j.name), detail));
                } else {
                    QMessageBox::critical(
                        &mw, title,
                        MainWindow::tr("\"%1\" failed.\n\n%2")
                            .arg(QString::fromStdString(j.name), detail));
                }
                break;
            }
        }, Qt::QueuedConnection);
    };
}

}  // namespace canvas::gui