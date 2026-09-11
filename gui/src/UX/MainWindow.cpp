#include "UX/MainWindow.hpp"
#include "Logging.hpp"

#include "ui_MainWindow.h"

#include "Widgets/media_pool_widget.hpp"

#include <QElapsedTimer>
<<<<<<< Updated upstream
#include <QFileDialog>
=======
#include <QFileInfo>
>>>>>>> Stashed changes
#include <QIcon>
#include <QImage>
#include <QPixmap>
#include <QStatusBar>
#include <QTimer>

#include <algorithm>
#include <memory>
#include <utility>

#include "core/timecode.hpp"

namespace canvas::gui {

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent) {
    new_untitled_project();
    build_ui();
    rebuild_recent_menu();

    // build_ui() creates the Deliver settings panel after the untitled project
    // above was set up, so seed it with the current timeline length here.
    if (deliver_settings_) {
        const double secs = total_frames_ > 0 && fps_ > 0.0
                                ? static_cast<double>(total_frames_) / fps_
                                : 0.0;
        deliver_settings_->set_timeline_length(secs, fps_);
    }

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
            [this](QStringList paths, int64_t frame, double scene_y) {
                const std::size_t start = project_->media.size();
                import_media_paths(paths);
                int64_t offset = 0;
                for (std::size_t i = start; i < project_->media.size(); ++i) {
                    place_media_at(project_->media[i].id, frame + offset, canvas::core::Placement::Overwrite, scene_y);
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
    // Scoped to this window only; a blanket app-level sheet would be too broad.
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
    if (deliver_settings_) {
        const double secs = total_frames_ > 0 && fps_ > 0.0
                                ? static_cast<double>(total_frames_) / fps_
                                : 0.0;
        deliver_settings_->set_timeline_length(secs, fps_);
    }
}

void MainWindow::push_snapshot(const int64_t initial_frame) {
    // Deep-copy the current project so the worker thread reads an immutable snapshot.
    const auto t0 = std::chrono::steady_clock::now();
    auto snapshot = std::make_shared<canvas::core::Project>(*project_);
    const double copy_ms = std::chrono::duration<double, std::milli>(
                               std::chrono::steady_clock::now() - t0).count();
    // Always-on: the copy cost lands on the UI thread on every edit. A big
    // timeline pushing multi-ms copies per keystroke shows up as edit lag even
    // when the worker keeps up, so this is the first place to look at.
    qWarning().nospace()
        << "[proj] snapshot push anchor=" << initial_frame
        << " copy_ms=" << QString::number(copy_ms, 'f', 1)
        << " undo_depth=" << undo_.count()
        << " last_cmd=" << (undo_.can_undo() ? QString::fromStdString(undo_.next_undo_name()) : QStringLiteral("-"))
        << " media=" << project_->media.size()
        << " v_tracks=" << project_->sequence.video_tracks.size()
        << " a_tracks=" << project_->sequence.audio_tracks.size()
        << " frames=" << project_->sequence.duration_frames();
    controller_.set_project(std::move(snapshot), initial_frame);
}

void MainWindow::push_audio_mix_snapshot() {
    if (!project_) return;
    auto snapshot = std::make_shared<canvas::core::Project>(*project_);
    controller_.update_audio_mix(std::move(snapshot));
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
<<<<<<< Updated upstream
=======
    // Starting playback is an explicit jump: if the user had scrolled away,
    // the playhead is centered under it again (per-frame position updates do
    // NOT re-enable follow, so a plain scroll mid-playback stays put).
    if (playing) timeline_->set_follow_playhead(true);
    viewer_->set_playing(playing);
>>>>>>> Stashed changes
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
    // Event-loop lag probe: schedule a zero-latency queued callback now and
    // measure how late it arrives. If anything blocks the main thread (synchronous
    // media open, a heavy paint), the 500ms timer fires late too, so the whole
    // stall accumulates here instead of being short-circuited by the next tick.
    const auto probe_t0 = std::chrono::steady_clock::now();
    QMetaObject::invokeMethod(this, [probe_t0] {
        static auto s_at = std::chrono::steady_clock::now();
        static int s_n = 0;
        static double s_ms = 0.0, s_max = 0.0;
        const double lag_ms = std::chrono::duration<double, std::milli>(
                                  std::chrono::steady_clock::now() - probe_t0).count();
        ++s_n;
        s_ms += lag_ms;
        s_max = std::max(s_max, lag_ms);
        const auto now = std::chrono::steady_clock::now();
        if (s_n == 1 || now - s_at >= std::chrono::seconds(2)) {
            s_at = now;
            qWarning().nospace()
                << "[eventloop] lag_avg_ms=" << QString::number(s_ms / s_n, 'f', 1)
                << " lag_max_ms=" << QString::number(s_max, 'f', 1)
                << " n=" << s_n;
            s_n = 0;
            s_ms = 0.0;
            s_max = 0.0;
        }
    }, Qt::QueuedConnection);
    // While a render job is running, the fps readout next to "Edited" doubles
    // as the encoder-speed meter instead of the playback rate.
    if (render_fps_ > 0.0) {
        fps_label_->setText(tr("%1 fps").arg(render_fps_, 0, 'f', 1));
        fps_label_->setStyleSheet(QStringLiteral("color: #4C92FF; font-size: 11px;"));
        return;
    }
    // The readout is the VIDEO's own frame cadence under the playhead (its
    // native media fps, 60 for 60fps footage on a 30fps timeline) — not the
    // present-to-present cadence, which only reflects the timeline's sequence
    // fps. The content itself strides at its intended rate by construction.
    fps_frames_ = 0;
    fps_clock_.restart();

    QString text;
    const QString color = QStringLiteral("#3DDC84");
    if (nominal_fps_ > 0.0) {
        text = QStringLiteral("%1 fps").arg(nominal_fps_, 0, 'f', 1);
    } else {
        text = tr("-- fps");
    }
    fps_label_->setText(text);
    fps_label_->setStyleSheet(QStringLiteral("color: %1; font-size: 11px;").arg(color));
}

void MainWindow::update_time_label() {
    const int64_t pos = current_frame_;
    time_label_->setText(timecode(pos, fps_) + QStringLiteral(" / ") + timecode(total_frames_, fps_));
}

}  // namespace canvas::gui
