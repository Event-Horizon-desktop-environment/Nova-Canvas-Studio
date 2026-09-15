#include "UX/MainWindow.hpp"
#include "UX/theme.hpp"
#include "Logging.hpp"

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

// ThumbnailService request id for the Dual-Viewer source preview's full-file
// audio spectrum. Media-pool cells use the media index and the timeline uses a
// monotonically growing counter, so the top bits keep it collision-free.
static constexpr std::uint64_t kSourcePreviewWaveformId = 0xF000000000000001ULL;

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent) {
    // The controller defaults to audible scrubbing on; the Settings dialog
    // persists the off-state, so honor it once here on startup.
    controller_.set_scrub_audio_enabled(
        QSettings().value(QStringLiteral("scrubAudioEnabled"), true).toBool());
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

    // Dual-Viewer source preview: the source controller presents straight into
    // its own ViewerGL (created in build_center_workspace). Cross-pause keeps
    // audio exclusive — only one controller holds the output device, so one
    // starting playback releases the other's device (see release_audio).
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
                // Always-on: source playback state is a top-level lifecycle event;
                // critical for diagnosing "hover scrub broke audio".
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
                    // Always-on: this is the moment a source tile becomes
                    // visible — the single most important diagnostic for
                    // "source preview is blank" reports.
                    qWarning().nospace()
                        << "[srcprv] media_changed has_media=1"
                        << " path=" << QString::fromStdString(src_preview_.media_path())
                        << " name=" << name
                        << " video=" << src_preview_.is_video()
                        << " audio=" << src_preview_.is_audio()
                        << " total_frames=" << src_preview_.total_frames()
                        << " fps=" << src_preview_.fps()
                        << " need_waveform=" << need_waveform;
                    // Audio-only media present as a spectrum, not a video frame:
                    // feed the panel the full-file waveform once per open (the
                    // worker reuses the cached raw buckets, so this is a cheap
                    // re-bucket+paint). The scrub playhead on top is live.
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
                // Hover-skim a pool tile: Live Media Preview only while the
                // Dual-Viewer source pane is actually visible (single mode
                // still paints the hover playhead, but decodes nothing).
                if (!source_panel_ || !source_panel_->isVisible()) {
                    // Throttled always-on: the single most common cause of
                    // "source preview doesn't work" — Dual-View is OFF (or the
                    // pane is collapsed), so every hover decodes nothing. Log at
                    // most once per second so normal single-mode browsing doesn't
                    // spiral, but one line proves the early-return was hit.
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
                    // Audible hover session start. A paused timeline still
                    // HOLDS the output device open, which would block the
                    // source's scrub grains — free it, but never cut a playing
                    // timeline (its playback keeps the device and the pool
                    // hover only previews video alongside it, Resolve-style).
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
            [this](int /*media_index*/) {
                if (!source_hovering_) return;
                source_hovering_ = false;
                // Audible-scrub session over: end_scrub CLOSES the source's
                // output device so the timeline can reopen it on its next Play.
                src_preview_.end_hover_scrub();
                qWarning() << "[srcprv] hover end (device released)";
            });

    connect(&thumbnails_, &ThumbnailService::thumbnail_ready, this,
            [this](uint64_t id, QImage image) {
                // Timeline filmstrip frames ride the same service; only
                // pool-namespaced ids may touch pool tiles (kPoolThumbNs).
                // Project-manager card frames carry their own high-bit prefix
                // (kProjectThumbNs) and are routed by the manager's bridge.
                if (!(id & kPoolThumbNs) || (id & kProjectThumbNs)) return;
                const int idx = static_cast<int>(id & ~kPoolThumbNs);
                qWarning().nospace() << "[thumb] pool thumbnail ready idx=" << idx
                                     << " sz=" << image.width() << "x" << image.height();
                if (media_pool_ && idx >= 0 && idx < media_pool_->count())
                    media_pool_->item(idx)->setIcon(QIcon(QPixmap::fromImage(image)));
            });
    connect(&thumbnails_, &ThumbnailService::waveform_ready, this,
            [this](uint64_t id, QImage image) {
                // Source-preview spectrum (audio-only media) comes back on its
                // own sentinel id — never a pool cell or timeline clip.
                if (id == kSourcePreviewWaveformId) {
                    qWarning().nospace() << "[thumb] source-preview waveform ready"
                                         << " sz=" << image.width() << "x" << image.height()
                                         << " null=" << image.isNull();
                    if (source_panel_) source_panel_->set_audio_waveform(image);
                    return;
                }
                // Timeline waveforms are handled by TimelineWidget's own
                // connection; only pool-namespaced ids route to pool tiles.
                if (!(id & kPoolThumbNs)) return;
                const int idx = static_cast<int>(id & ~kPoolThumbNs);
                qWarning().nospace() << "[thumb] pool waveform ready idx=" << idx
                                     << " sz=" << image.width() << "x" << image.height()
                                     << " null=" << image.isNull();
                if (media_pool_ && idx >= 0 && idx < media_pool_->count()) {
                    QListWidgetItem* item = media_pool_->item(idx);
                    // Hybrid video+audio tiles keep the frame as the icon (top)
                    // and stash this spectrum for the bottom strip; audio-only
                    // media still use it as the whole-tile preview.
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

    // Keep the dock chrome (separators, drop-shadows, dock title areas) in
    // sync with the active appearance tokens. Scoped to this window only; a
    // blanket app-level sheet would be too broad.
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

    // First screen: the Project Manager, presented as its own floating window.
    // Its thumbnail bridge is fully wired by enter_project_manager(), so the
    // first refresh can resolve card thumbnails.
    enter_project_manager();
}

MainWindow::~MainWindow() {
    // A subtitle worker past the dialog close must never outlive the window:
    // join before the members it reports into are destroyed.
    if (subtitle_worker_.joinable()) subtitle_worker_.join();
    delete ui;
}

void MainWindow::refresh_timeline() {
    // Every edit (and project load) resyncs the time basis: total length AND
    // the frame-rate used to render that length. fps_ is the readout rate for
    // the transport/labels/Deliver — leaving it at the constructor default of
    // 30 while the sequence adopts the media's own rate made the time read
    // wrong after edits against the video and the ruler.
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
    // Deep-copy the current project so the worker thread reads an immutable snapshot.
    const auto t0 = std::chrono::steady_clock::now();
    auto snapshot = std::make_shared<canvas::core::Project>(*project_);
    const double copy_ms = std::chrono::duration<double, std::milli>(
                               std::chrono::steady_clock::now() - t0).count();
    // Always-on: the copy cost lands on the UI thread on every edit. A big
    // timeline pushing multi-ms copies per keystroke shows up as edit lag even
    // when the worker keeps up, so this is the first place to look at.
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
    // Grade-only snapshot for the Color page. A plain push_snapshot() would fire
    // SetProject, which tears down + rebuilds the entire decode stack (~217ms per
    // tick — the reason the page's preview used to run at ~4Hz). swap_project
    // keeps the decoders warm and just re-presents the current frame with the new
    // 3D-LUT grade, making wheel/curve previews effectively realtime.
    auto snapshot = std::make_shared<canvas::core::Project>(*project_);
    controller_.swap_project(std::move(snapshot));
}

void MainWindow::push_live_snapshot() {
    if (!project_) return;
    // Live inspector-preview snapshot (Subtitles page slider drags): like
    // push_grade_snapshot, push via swap_project so the current frame re-presents
    // through the warm decoders instead of paying set_project()'s decode teardown
    // per slider tick — that is what makes the preview feel realtime mid-drag.
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
    // Starting playback is an explicit jump: if the user had scrolled away,
    // the playhead is centered under it again (per-frame position updates do
    // NOT re-enable follow, so a plain scroll mid-playback stays put).
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
            qDebug().nospace()
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
        fps_label_->setStyleSheet(QStringLiteral("color: %1; font-size: 11px;")
                                      .arg(css(tokens().accent_hover)));
        return;
    }
    // The readout is the VIDEO's own frame cadence under the playhead (its
    // native media fps, 60 for 60fps footage on a 30fps timeline) — not the
    // present-to-present cadence, which only reflects the timeline's sequence
    // fps. The content itself strides at its intended rate by construction.
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

}  // namespace canvas::gui
