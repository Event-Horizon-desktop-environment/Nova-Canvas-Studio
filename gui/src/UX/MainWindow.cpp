#include "UX/MainWindow.hpp"
#include "UX/theme.hpp"
#include "Logging.hpp"

#include "canvas/core/project/autosave.hpp"

#include "ui_MainWindow.h"

#include "Widgets/media_pool_widget.hpp"
#include "Widgets/viewer_gl.hpp"

#include <QElapsedTimer>
#include <QFileInfo>
#include <QIcon>
#include <QImage>
#include <QPixmap>
#include <QSettings>
#include <QStatusBar>
#include <QTimer>

#include <algorithm>
#include <cstdint>
#include <memory>
#include <utility>

#include "core/timecode.hpp"

namespace canvas::gui {

static constexpr std::uint64_t kSourcePreviewWaveformId = 0xF000000000000001ULL;

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent) {
    controller_.set_scrub_audio_enabled(
        QSettings().value(QStringLiteral("scrubAudioEnabled"), true).toBool());
    new_untitled_project();
    build_ui();
    rebuild_recent_menu();

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

    connect(&src_preview_, &source_preview::SourcePreviewController::frame_ready, this,
            [this](canvas::core::RenderFramePtr frame) {
                if (source_panel_) {
                    if (debug_enabled()) {
                        const bool has_a = frame && frame->a && !frame->a->rgba.empty();
                        qDebug().nospace() << "[srcprv] frame_ready"
                                           << " has_a=" << has_a;
                    }
                    source_panel_->viewer()->set_frame(std::move(frame));
                }
            });
    connect(&src_preview_, &source_preview::SourcePreviewController::position_changed, this,
            [this](int64_t frame) {
                if (source_panel_) source_panel_->set_media_position(frame, src_preview_.fps());
            });
    connect(&src_preview_, &source_preview::SourcePreviewController::playback_changed, this,
            [this](bool playing) {
                if (source_panel_) source_panel_->set_playing(playing);
                qWarning().nospace() << "[srcprv] playback playing=" << playing;
                if (playing) controller_.release_audio();
            });
    connect(&src_preview_, &source_preview::SourcePreviewController::media_changed, this,
            [this](bool has_media) {
                if (!source_panel_) return;
                if (!has_media) {
                    source_panel_->clear_media();
                    qWarning() << "[srcprv] media_changed has_media=0 clear";
                } else {
                    const QString name = QFileInfo(QString::fromStdString(src_preview_.media_path())).completeBaseName();
                    source_panel_->set_media_info(
                        name,
                        src_preview_.is_video(), src_preview_.is_audio(),
                        src_preview_.total_frames());
                    const bool need_waveform = src_preview_.is_audio() && !src_preview_.is_video();
                    qWarning().nospace()
                        << "[srcprv] media_changed has_media=1"
                        << " path=" << QString::fromStdString(src_preview_.media_path())
                        << " name=" << name
                        << " video=" << src_preview_.is_video()
                        << " audio=" << src_preview_.is_audio()
                        << " total_frames=" << src_preview_.total_frames()
                        << " fps=" << src_preview_.fps()
                        << " need_waveform=" << need_waveform;
                    if (need_waveform)
                        thumbnails_.request_waveform(kSourcePreviewWaveformId,
                                                     src_preview_.media_path(),
                                                     2048, 260, 0.0f, 1.0f);
                }
            });
    connect(&controller_, &SequenceController::playback_changed, this, [this](bool playing) {
        if (playing) src_preview_.release_audio();
    });
    connect(media_pool_, &MediaPoolWidget::clipScrubbed, this,
            [this](int media_index, double fraction) {
                if (!source_panel_ || !source_panel_->isVisible()) {
                    static auto last_ignored = std::chrono::steady_clock::now();
                    const auto now = std::chrono::steady_clock::now();
                    if (std::chrono::duration_cast<std::chrono::milliseconds>(now - last_ignored).count() >= 1000) {
                        qWarning() << "[srcprv] pool hover IGNORED (source panel hidden) idx="
                                   << media_index
                                   << " frac=" << fraction;
                        last_ignored = now;
                    }
                    return;
                }
                if (!project_ || media_index < 0 ||
                    static_cast<std::size_t>(media_index) >= project_->media.size())
                    return;
                if (!source_hovering_) {
                    if (!controller_.is_playing()) controller_.release_audio();
                    src_preview_.begin_hover_scrub();
                    source_hovering_ = true;
                    qWarning() << "[srcprv] hover begin idx=" << media_index
                               << " path=" << QString::fromStdString(project_->media[static_cast<std::size_t>(media_index)].path);
                }
                open_source_preview(project_->media[static_cast<std::size_t>(media_index)]);
                if (debug_enabled())
                    qDebug().nospace() << "[srcprv] hover move idx=" << media_index
                                       << " frac=" << fraction;
                src_preview_.scrub_fraction(fraction);
            });
    connect(media_pool_, &MediaPoolWidget::clipScrubEnded, this,
            [this](int) {
                if (!source_hovering_) return;
                source_hovering_ = false;
                src_preview_.end_hover_scrub();
                qWarning() << "[srcprv] hover end (device released)";
            });

    connect(&thumbnails_, &ThumbnailService::thumbnail_ready, this,
            [this](uint64_t id, QImage image) {
                if ((id & kProjectThumbNs) != kPoolThumbNs) return;
                const int idx = static_cast<int>(id & ~kPoolThumbNs);
                if (debug_enabled())
                    qWarning().nospace() << "[thumb] pool thumbnail ready idx=" << idx
                                         << " sz=" << image.width() << "x" << image.height();
                if (media_pool_ && idx >= 0 && idx < media_pool_->count())
                    media_pool_->item(idx)->setIcon(QIcon(QPixmap::fromImage(image)));
            });
    connect(&thumbnails_, &ThumbnailService::waveform_ready, this,
            [this](uint64_t id, QImage image) {
                if (id == kSourcePreviewWaveformId) {
                    if (debug_enabled())
                        qWarning().nospace() << "[thumb] source-preview waveform ready"
                                             << " sz=" << image.width() << "x" << image.height()
                                             << " null=" << image.isNull();
                    if (source_panel_) source_panel_->set_audio_waveform(image);
                    return;
                }
                if ((id & kProjectThumbNs) != kPoolThumbNs) return;
                const int idx = static_cast<int>(id & ~kPoolThumbNs);
                if (debug_enabled())
                    qWarning().nospace() << "[thumb] pool waveform ready idx=" << idx
                                         << " sz=" << image.width() << "x" << image.height()
                                         << " null=" << image.isNull();
                if (media_pool_ && idx >= 0 && idx < media_pool_->count()) {
                    QListWidgetItem* item = media_pool_->item(idx);
                    if (item->data(kPoolIsVideoRole).toBool() &&
                        item->data(kPoolHasAudioRole).toBool())
                        item->setData(kPoolWaveformImageRole, image);
                    else
                        item->setIcon(QIcon(QPixmap::fromImage(image)));
                }
            });

    setWindowTitle(tr("Nova Canvas Studio"));
    resize(1440, 860);
    status_->showMessage(tr("Import media with File > Import Media (Ctrl+I)"));

    apply_theme_style(this, [] {
        const ThemeTokens& t = tokens();
        return QStringLiteral(
            "QMainWindow { background: %1; }"
            "QMainWindow::separator { background: %2; width: 2px; height: 2px; }"
            "QDockWidget { background: %3; color: %4; }"
            "QDockWidget::title { background: %5; color: %4; padding: 4px 8px; "
            "border: none; text-align: center; }")
.arg(css(t.surface), css(t.border), css(t.surface_low), css(t.ink),
                  css(t.surface_raised));
    });

    autosave_timer_ = new QTimer(this);
    autosave_timer_->setInterval(canvas::core::autosave::Policy{}.interval_seconds * 1000);
    connect(autosave_timer_, &QTimer::timeout, this, &MainWindow::maybe_autosave);
    autosave_timer_->start();

    enter_project_manager();
}

MainWindow::~MainWindow() {
    if (subtitle_worker_.joinable()) subtitle_worker_.join();
    delete ui;
}

void MainWindow::refresh_timeline() {
    fps_ = project_->sequence.fps > 0.0 ? project_->sequence.fps : 30.0;
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
    const auto t0 = std::chrono::steady_clock::now();
    auto snapshot = std::make_shared<canvas::core::Project>(*project_);
    const double copy_ms = std::chrono::duration<double, std::milli>(
                               std::chrono::steady_clock::now() - t0).count();
    qDebug().nospace()
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

void MainWindow::push_grade_snapshot() {
    if (!project_) return;
    auto snapshot = std::make_shared<canvas::core::Project>(*project_);
    controller_.swap_project(std::move(snapshot));
}

void MainWindow::push_live_snapshot() {
    if (!project_) return;
    auto snapshot = std::make_shared<canvas::core::Project>(*project_);
    controller_.swap_project(std::move(snapshot));
}

void MainWindow::open_source_preview(const canvas::core::MediaEntry& media) {
    src_preview_.open_media(media, project_ ? project_->sequence.fps : 30.0);
}

void MainWindow::clear_source_preview() {
    src_preview_.close_media();
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
    if (playing) timeline_->set_follow_playhead(true);
    viewer_->set_playing(playing);
    const QString icon_path = playing ? QStringLiteral(":/icons/pause.svg")
                                      : QStringLiteral(":/icons/play.svg");
    play_button_->setIcon(QIcon(icon_path));
    if (!playing) {
        fps_frames_ = 0;
        fps_clock_.restart();
    }
}

void MainWindow::update_fps_label() {
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
            qDebug().nospace()
                << "[eventloop] lag_avg_ms=" << QString::number(s_ms / s_n, 'f', 1)
                << " lag_max_ms=" << QString::number(s_max, 'f', 1)
                << " n=" << s_n;
            s_n = 0;
            s_ms = 0.0;
            s_max = 0.0;
        }
    }, Qt::QueuedConnection);
    if (render_fps_ > 0.0) {
        fps_label_->setText(tr("%1 fps").arg(render_fps_, 0, 'f', 1));
        fps_label_->setStyleSheet(QStringLiteral("color: %1; font-size: 11px;")
                                      .arg(css(tokens().accent_hover)));
        return;
    }
    fps_frames_ = 0;
    fps_clock_.restart();

    QString text;
    const QString color = css(tokens().accent);
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

}