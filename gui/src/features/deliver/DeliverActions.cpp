#include "UX/MainWindow.hpp"

#include "features/deliver/deliver_settings_panel.hpp"
#include "features/deliver/render_queue_panel.hpp"

#include "canvas/core/export/deliver_preset.hpp"
#include "canvas/core/export/edl.hpp"
#include "canvas/core/export/render_queue.hpp"

#include <QDir>
#include <QDockWidget>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QStandardPaths>
#include <QStatusBar>

#include <algorithm>
#include <memory>
#include <string>
#include <utility>

namespace canvas::gui {

namespace {

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

}

void MainWindow::enter_deliver_page() {
    leave_project_manager();
    deliver_active_ = true;
    if (media_dock_) media_dock_->hide();
    if (inspector_dock_) inspector_dock_->hide();
    if (deliver_settings_dock_) deliver_settings_dock_->show();
    if (deliver_queue_dock_) deliver_queue_dock_->show();
    reflect_render_queue();
    status_->showMessage(tr("Deliver: configure settings and add to the render queue."));
}

void MainWindow::enter_edit_page() {
    leave_project_manager();
    deliver_active_ = false;
    if (deliver_settings_dock_) deliver_settings_dock_->hide();
    if (deliver_queue_dock_) deliver_queue_dock_->hide();
    if (media_dock_) media_dock_->show();
    if (inspector_dock_ && inspector_dock_->isVisible()) {}
    status_->showMessage(tr("Import media with File > Import Media (Ctrl+I)"));
}

void MainWindow::reflect_render_queue() {
    if (deliver_queue_panel_)
        deliver_queue_panel_->set_project_name(
            project_ && project_->name != "Untitled Project"
                ? QString::fromStdString(project_->name)
                : QString());
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
        ds.video.resolution = std::to_string(ds.video.custom_width) + " x "
                            + std::to_string(ds.video.custom_height);
    }
    if (ds.video.frame_rate == "Auto") {
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
        {
            char buf[16];
            std::snprintf(buf, sizeof(buf), "%.4g", f);
            ds.video.frame_rate = buf;
        }
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

void MainWindow::export_edl() {
    if (!project_) return;

    const QString base_dir =
        project_path_.isEmpty()
            ? QStandardPaths::writableLocation(QStandardPaths::MoviesLocation)
            : QFileInfo(project_path_).absolutePath();
    const QString stem = project_->name.empty() ? QStringLiteral("timeline")
                                                : QString::fromStdString(project_->name);
    const QString suggested = QDir(base_dir).filePath(stem + QStringLiteral(".edl"));

    const QString path = QFileDialog::getSaveFileName(
        this, tr("Export EDL"), suggested,
        tr("Edit Decision List (*.edl);;All Files (*)"));
    if (path.isEmpty()) return;

    const std::string text =
        canvas::core::edl::write_cmx3600(project_->sequence, project_->name, project_->media);

    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        status_->showMessage(tr("Could not write EDL: %1").arg(path));
        return;
    }
    file.write(text.data(), static_cast<qint64>(text.size()));
    file.close();
    status_->showMessage(tr("Exported EDL: %1").arg(path));
}

}
