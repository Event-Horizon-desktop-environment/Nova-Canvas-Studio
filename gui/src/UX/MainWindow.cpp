#include "UX/MainWindow.hpp"
#include "Logging.hpp"

#include "ui_MainWindow.h"

#include "Widgets/media_pool_widget.hpp"

#include <QDir>
#include <QElapsedTimer>
#include <QFileDialog>
#include <QIcon>
#include <QImage>
#include <QKeyEvent>
#include <QMessageBox>
#include <QPixmap>
#include <QStandardPaths>
#include <QStatusBar>
#include <QStyle>
#include <QTimer>

#include <algorithm>
#include <memory>
#include <utility>

#include "core/timecode.hpp"

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

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent) {
    new_untitled_project();
    build_ui();
    rebuild_recent_menu();

    fps_clock_.start();
    fps_timer_ = new QTimer(this);
    fps_timer_->setInterval(500);
    connect(fps_timer_, &QTimer::timeout, this, &MainWindow::on_fps_tick);
    fps_timer_->start();

    // Drag media files in from a file browser: into the media pool -> import
    // only; onto the timeline -> import AND place at the drop frame.
    connect(media_pool_, &MediaPoolWidget::filesDropped, this,
            [this](QStringList paths) { import_media_paths(paths); });
    connect(timeline_, &TimelineWidget::media_files_dropped, this,
            [this](QStringList paths, int64_t frame) {
                const std::size_t start = project_->media.size();
                import_media_paths(paths);
                int64_t offset = 0;
                for (std::size_t i = start; i < project_->media.size(); ++i) {
                    place_media_at(project_->media[i].id, frame + offset, canvas::core::Placement::Overwrite);
                    const auto& m = project_->media[i];
                    offset += (m.total_frames > 0 ? m.total_frames : 300 * static_cast<int64_t>(m.fps > 0 ? m.fps : 30.0));
                }
            });

    connect(&controller_, &SequenceController::frame_ready, this,
            [this](canvas::core::RenderFramePtr frame) {
                ++fps_frames_;
                viewer_->set_frame(std::move(frame));
            });
    connect(&controller_, &SequenceController::position_changed, this, &MainWindow::on_position_changed);
    connect(&controller_, &SequenceController::playback_changed, this, &MainWindow::on_playback_changed);

    connect(&thumbnails_, &ThumbnailService::thumbnail_ready, this,
            [this](uint64_t id, QImage image) {
                const int idx = static_cast<int>(id);
                if (media_pool_ && idx >= 0 && idx < media_pool_->count()) {
                    media_pool_->item(idx)->setIcon(QIcon(QPixmap::fromImage(image)));
                }
            });
    connect(&thumbnails_, &ThumbnailService::waveform_ready, this,
            [this](uint64_t id, QImage image) {
                const int idx = static_cast<int>(id);
                if (media_pool_ && idx >= 0 && idx < media_pool_->count()) {
                    media_pool_->item(idx)->setIcon(QIcon(QPixmap::fromImage(image)));
                }
            });

    setWindowTitle(tr("Nova Canvas Studio"));
    resize(1440, 860);
    status_->showMessage(tr("Import media with File > Import Media (Ctrl+I)"));

    // Keep the dock chrome (separators, drop-shadows, dock title areas) dark.
    // Scoped to this window only; a blanket app-level sheet would override
    // qlementine.
    setStyleSheet(QStringLiteral(
        "QMainWindow { background: #11131A; }"
        "QMainWindow::separator { background: #2A2F3C; width: 2px; height: 2px; }"
        "QDockWidget { background: #141A21; color: #E8EAF0; }"
        "QDockWidget::title { background: #1A1D27; color: #E8EAF0; padding: 4px 8px; "
        "border: none; text-align: center; }"));
}

MainWindow::~MainWindow() { delete ui; }

void MainWindow::refresh_timeline() {
    timeline_->set_sequence(&project_->sequence);
    total_frames_ = project_->sequence.duration_frames();
    scrub_->setRange(0, static_cast<int>(std::max<int64_t>(total_frames_ - 1, 0)));
}

void MainWindow::push_snapshot(const int64_t initial_frame) {
    // Deep-copy the current project so the worker thread reads an immutable snapshot.
    qWarning().nospace()
        << "[proj] snapshot push anchor=" << initial_frame
        << " undo_depth=" << undo_.count()
        << " last_cmd=" << (undo_.can_undo() ? QString::fromStdString(undo_.next_undo_name()) : QStringLiteral("-"))
        << " media=" << project_->media.size()
        << " v_tracks=" << project_->sequence.video_tracks.size()
        << " a_tracks=" << project_->sequence.audio_tracks.size()
        << " frames=" << project_->sequence.duration_frames();
    auto snapshot = std::make_shared<canvas::core::Project>(*project_);
    controller_.set_project(std::move(snapshot), initial_frame);
}

void MainWindow::on_position_changed(const int64_t frame_number) {
    current_frame_ = frame_number;
    viewer_->set_mode(ViewerGL::ViewerMode::Program);
    if (!scrub_->isSliderDown()) scrub_->setValue(static_cast<int>(frame_number));
    timeline_->set_playhead_position(frame_number);
    update_time_label();
    update_fps_label();
}

void MainWindow::on_playback_changed(const bool playing) {
    play_button_->setText(playing ? tr("Pause") : tr("Play"));
    const QString icon_path = playing ? QStringLiteral(":/icons/pause.svg")
                                      : QStringLiteral(":/icons/play.svg");
    play_button_->setIcon(QIcon(icon_path));
    if (!playing) {
        fps_frames_ = 0;
        fps_clock_.restart();
    }
}

void MainWindow::update_fps_label() {
    // Determine the nominal frame rate of the video under the playhead,
    // mirroring the same top-down video-track lookup the playback uses.
    nominal_fps_ = 0.0;
    if (!project_) return;
    const auto& seq = project_->sequence;
    for (std::size_t i = seq.video_tracks.size(); i-- > 0;) {
        const auto& track = seq.video_tracks[i];
        if (track.locked) continue;
        const canvas::core::Clip* clip = track.clip_at(current_frame_);
        if (!clip) continue;
        if (const canvas::core::MediaEntry* m = project_->media_by_id(clip->media)) {
            if (m->fps > 0.0) { nominal_fps_ = m->fps; break; }
        }
    }
    if (nominal_fps_ <= 0.0) nominal_fps_ = seq.fps;
}

void MainWindow::on_fps_tick() {
    if (!fps_label_) return;
    const qint64 ms = fps_clock_.restart();
    double live = 0.0;
    if (ms > 0 && fps_frames_ > 0) live = fps_frames_ * 1000.0 / ms;
    fps_frames_ = 0;

    QString text;
    QString color = QStringLiteral("#9AA0B0");
    if (nominal_fps_ > 0.0 && live > 0.0) {
        text = QStringLiteral("%1 fps").arg(live, 0, 'f', 0);
        color = live >= nominal_fps_ - 0.5
                    ? QStringLiteral("#3DDC84")  // green: at/near full video fps
                    : QStringLiteral("#FF4B4B");  // red: below the video's fps
    } else {
        text = nominal_fps_ > 0.0 ? tr("%1 fps").arg(nominal_fps_, 0, 'f', 1)
                                  : tr("-- fps");
    }
    fps_label_->setText(text);
    fps_label_->setStyleSheet(QStringLiteral("color: %1; font-size: 11px;").arg(color));
}

void MainWindow::update_time_label() {
    const int64_t pos = current_frame_;
    time_label_->setText(timecode(pos, fps_) + QStringLiteral(" / ") + timecode(total_frames_, fps_));
}

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
    // Update the running render's speed in the settings panel.
    const auto jobs = render_queue_.jobs();
    for (const auto& j : jobs) {
        if (j.status == canvas::core::RenderJob::Status::Rendering) {
            const bool gpu = j.settings.video.encoder == canvas::core::EncoderBackend::NVIDIA ||
                             j.settings.video.encoder == canvas::core::EncoderBackend::AMD ||
                             j.settings.video.encoder == canvas::core::EncoderBackend::Intel;
            if (deliver_settings_) deliver_settings_->set_render_speed(j.render_fps, gpu);
            return;
        }
    }
    if (deliver_settings_) deliver_settings_->set_render_speed(0.0, false);
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
