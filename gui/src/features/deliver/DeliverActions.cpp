// Deliver-page actions on MainWindow: page enter/exit (dock visibility), render
// queue reflection, and the two user-facing render actions (add current to
// queue, render all). Split out of MainWindow.cpp as part of the splitplan
// refactor so the deliver feature lives under features/deliver/ next to its
// model + panels.

#include "UX/MainWindow.hpp"

#include "features/deliver/deliver_settings_panel.hpp"
#include "features/deliver/render_queue_panel.hpp"

#include "canvas/core/export/deliver_preset.hpp"
#include "canvas/core/export/render_queue.hpp"

#include <QDir>
#include <QDockWidget>
#include <QStandardPaths>
#include <QStatusBar>

#include <algorithm>
#include <memory>
#include <string>
#include <utility>

namespace canvas::gui {

namespace {

// Best-effort file extension for a Deliver format display name.
QString extension_for_format(const std::string& format) {
    const std::string f = format;
    const auto has = [&](const char* s) { return f.find(s) != std::string::npos; };
    if (has("MKV")) return QStringLiteral("mkv");
    if (has("MP4")) return QStringLiteral("mp4");
    if (has("QuickTime")) return QStringLiteral("mov");
    if (has("WebM")) return QStringLiteral("webm");
    if (has("AVI")) return QStringLiteral("avi");
    if (has("GIF")) return QStringLiteral("gif");
    if (has("PNG")) return QStringLiteral("png");
    if (has("TIFF")) return QStringLiteral("tif");
    if (has("JPEG")) return QStringLiteral("jpg");
    if (has("WebP")) return QStringLiteral("webp");
    if (has("DPX")) return QStringLiteral("dpx");
    if (has("EXR")) return QStringLiteral("exr");
    if (has("MXF")) return QStringLiteral("mxf");
    if (has("MPEG")) return QStringLiteral("mpeg");
    return QStringLiteral("mkv");
}

}  // namespace

void MainWindow::enter_deliver_page() {
    deliver_active_ = true;
    if (media_dock_) media_dock_->hide();
    if (inspector_dock_) inspector_dock_->hide();
    if (deliver_settings_dock_) deliver_settings_dock_->show();
    if (deliver_queue_dock_) deliver_queue_dock_->show();
    reflect_render_queue();
    status_->showMessage(tr("Deliver: configure settings and add to the render queue."));
}

void MainWindow::enter_edit_page() {
    deliver_active_ = false;
    if (deliver_settings_dock_) deliver_settings_dock_->hide();
    if (deliver_queue_dock_) deliver_queue_dock_->hide();
    if (media_dock_) media_dock_->show();
    if (inspector_dock_ && inspector_dock_->isVisible()) { /* keep user state */ }
    status_->showMessage(tr("Import media with File > Import Media (Ctrl+I)"));
}

void MainWindow::reflect_render_queue() {
    if (deliver_queue_panel_) deliver_queue_panel_->refresh();
    // Flag the top-bar fps readout for render-speed mode while a job runs;
    // on_fps_tick picks it up on its next pulse. The count comes straight from
    // the queue — no duplicated label in the settings panel anymore.
    render_fps_ = 0.0;
    for (const auto& j : render_queue_.jobs()) {
        if (j.status == canvas::core::RenderJob::Status::Rendering) {
            render_fps_ = j.render_fps;
            break;
        }
    }
}

void MainWindow::add_current_to_render_queue() {
    if (!project_) return;

    canvas::core::DeliverSettings ds = deliver_settings_->settings();

    // Resolve "Timeline Resolution" / "Timeline Frame Rate" to concrete values.
    if (ds.video.resolution == "Timeline Resolution") {
        int w = 0, h = 0;
        for (const auto& track : project_->sequence.video_tracks) {
            for (const auto& clip : track.clips) {
                if (!clip.enabled) continue;
                if (const canvas::core::MediaEntry* m = project_->media_by_id(clip.media)) {
                    if (m->width > w && m->height > h) { w = m->width; h = m->height; }
                }
            }
        }
        ds.video.custom_width = w > 0 ? w : 1920;
        ds.video.custom_height = h > 0 ? h : 1080;
    }
    if (ds.video.frame_rate == "Auto") {
        // Auto-detect the source media frame rate (the highest fps among the
        // enabled clips on the timeline), so e.g. 60fps sources render at 60fps
        // without the user having to set it manually.
        double f = 0.0;
        for (const auto& track : project_->sequence.video_tracks) {
            for (const auto& clip : track.clips) {
                if (!clip.enabled) continue;
                if (const canvas::core::MediaEntry* m = project_->media_by_id(clip.media))
                    f = std::max(f, m->fps);
            }
        }
        if (f <= 0.0) f = project_->sequence.fps;
        if (f <= 0.0) f = 30.0;
        ds.video.custom_fps = f;
    }

    canvas::core::ExportSettings es = canvas::core::to_export_settings(ds);

    QString dir = QString::fromStdString(ds.file.location);
    if (dir.trimmed().isEmpty())
        dir = QStandardPaths::writableLocation(QStandardPaths::MoviesLocation);
    const QString ext = extension_for_format(ds.video.format);
    const QString out_path =
        QDir(dir).filePath(QString::fromStdString(ds.file.file_name) + QStringLiteral(".") + ext);

    render_queue_.set_active_project(std::make_shared<const canvas::core::Project>(*project_), {});

    if (ds.render_scope == canvas::core::RenderScope::IndividualClips) {
        // Enqueue one job per video clip on the timeline.
        int index = 0;
        for (const auto& track : project_->sequence.video_tracks) {
            for (const auto& clip : track.clips) {
                if (!clip.enabled || clip.media < 0) continue;
                canvas::core::RenderJob job;
                job.name = (QString::fromStdString(ds.file.file_name) +
                            QStringLiteral("_clip%1").arg(index + 1))
                               .toStdString();
                job.settings = ds;
                job.output_path =
                    QDir(dir).filePath(QString::fromStdString(job.name) + QStringLiteral(".") + ext)
                        .toStdString();
                job.total_frames = clip.duration();
                render_queue_.enqueue(std::move(job));
                ++index;
            }
        }
    } else {
        canvas::core::RenderJob job;
        job.name = ds.file.file_name;
        job.settings = ds;
        job.output_path = out_path.toStdString();
        job.total_frames = controller_.total_frames();
        render_queue_.enqueue(std::move(job));
    }

    reflect_render_queue();
    has_unsaved_changes_ = true;
    status_->showMessage(tr("Added render job(s) to the queue."));
}

void MainWindow::render_all_from_queue() {
    // Explicit user action: start draining every queued job. Enqueueing alone
    // only stages work; rendering begins here.
    int queued = 0;
    const auto jobs = render_queue_.jobs();
    for (const auto& j : jobs)
        if (j.status == canvas::core::RenderJob::Status::Queued) ++queued;
    if (queued > 0) {
        render_queue_.start();
        status_->showMessage(tr("Rendering %1 queued job(s)...").arg(queued));
    } else {
        status_->showMessage(tr("Nothing queued to render. Add a job to the queue first."));
    }
    reflect_render_queue();
}

}  // namespace canvas::gui