#include "UX/MainWindow.hpp"
#include "UX/InspectorAudio.hpp"
#include "UX/InspectorFile.hpp"
#include "UX/theme.hpp"
#include "Logging.hpp"

#include "Widgets/media_pool_widget.hpp"
#include "features/color/mini_timeline_strip.hpp"

#include <QFileDialog>
#include <QFileInfo>
#include <QMenu>
#include <QSettings>
#include <QFont>
#include <QIcon>
#include <QImage>
#include <QLabel>
#include <QListWidget>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QColor>
#include <QStatusBar>

#include <chrono>
#include <QSize>

#include <algorithm>
#include <cmath>
#include <string>
#include <unordered_map>
#include <utility>

namespace canvas::gui {

void MainWindow::new_untitled_project() {
    auto p = std::make_unique<canvas::core::Project>();
    p->name = "Untitled Project";
    canvas::core::Track v1;
    v1.kind = canvas::core::Track::Kind::Video;
    v1.name = "V1";
    canvas::core::Track a1;
    a1.kind = canvas::core::Track::Kind::Audio;
    a1.name = "A1";
    p->sequence.video_tracks.push_back(std::move(v1));
    p->sequence.audio_tracks.push_back(std::move(a1));
    project_ = std::move(p);
    undo_.clear();
    current_bin_.clear();
    project_->bins.clear();
    if (color_mini_strip_) color_mini_strip_->set_sequence(&project_->sequence);
}

void MainWindow::ensure_tracks_at(canvas::core::Track::Kind kind, std::size_t index) {
    auto& tracks = kind == canvas::core::Track::Kind::Video ? project_->sequence.video_tracks
                                                            : project_->sequence.audio_tracks;
    while (tracks.size() <= index) {
        canvas::core::Track t;
        t.kind = kind;
        t.name = (kind == canvas::core::Track::Kind::Video ? "V" : "A") +
                 std::to_string(tracks.size() + 1);
        tracks.push_back(std::move(t));
    }
}

bool MainWindow::place_selected_media(canvas::core::Placement mode) {
    if (!project_ || media_pool_->currentRow() < 0) return false;
    QListWidgetItem* item = media_pool_->currentItem();
    if (!item) return false;
    const QVariant v = item->data(Qt::UserRole);
    if (!v.isValid()) return false;
    const int idx = static_cast<int>(v.toLongLong());
    if (static_cast<std::size_t>(idx) >= project_->media.size()) return false;
    return place_media_at(project_->media[idx].id, current_frame_, mode);
}

bool MainWindow::place_media_at(canvas::core::MediaId media_id, int64_t frame,
                                canvas::core::Placement mode,
                                std::optional<double> drop_scene_y) {
    if (!project_) return false;
    // A drop over the track header (or anywhere left of the timeline's first
    // frame) yields a negative frame from the widget's pixel→frame map; clamp
    // it so the placed clip flushes against the timeline start instead of
    // painting on top of the header strip. frame_at_x callers elsewhere floor
    // ≥0 themselves; this funnel guards every placement path.
    frame = std::max<int64_t>(0, frame);
    const canvas::core::MediaEntry* found = nullptr;
    for (const auto& m : project_->media) {
        if (m.id == media_id) { found = &m; break; }
    }
    if (!found) return false;

    const bool audio_only = found->width <= 0 && found->height <= 0;
    const int64_t src_out = found->total_frames > 0
                                ? found->total_frames
                                : 300 * static_cast<int64_t>(found->fps > 0 ? found->fps : 30.0);
    const std::string base = QFileInfo(QString::fromStdString(found->path))
                                 .completeBaseName()
                                 .toStdString();
    // The lane the drop targets: an existing channel of the media's kind under
    // the drop point, or the per-kind index of a channel to CREATE there (drop
    // into empty space). Without a drop point (pool double-click, menu) fall
    // back to the first channel of the kind.
    const auto kind = audio_only ? canvas::core::Track::Kind::Audio : canvas::core::Track::Kind::Video;
    const int lane = drop_scene_y && timeline_
                         ? timeline_->resolve_drop_lane(*drop_scene_y, kind).index
                         : 0;
    ensure_tracks_at(kind, static_cast<std::size_t>(lane));

    // Pure audio media (mp3/flac/wav/...): lands as a single audio clip on the
    // dropped audio lane. Creating a linked video half would put a dead video
    // clip (no decode) on a video channel, so audio-only media stays audio-only.
    if (audio_only) {
        canvas::core::Clip aclip;
        aclip.media = found->id;
        aclip.tl_in = frame;
        aclip.src_in = 0;
        aclip.src_out = src_out;
        aclip.name = base + " Audio";
        auto cmd = canvas::core::place_clip(project_->sequence, canvas::core::Track::Kind::Audio,
                                        static_cast<std::size_t>(lane), std::move(aclip), mode,
                                        found->fps);
        if (!cmd) return false;
        qWarning() << "[edit] PLACE-AUDIO media=" << media_id << "track=" << lane
                   << "at=" << frame << "mode=" << static_cast<int>(mode)
                   << "path=" << QString::fromStdString(found->path);
        undo_.record(std::move(cmd));
        has_unsaved_changes_ = true;
        refresh_timeline();
        push_snapshot();
        return true;
    }

    canvas::core::Clip clip;
    clip.media = found->id;
    clip.tl_in = frame;
    clip.src_in = 0;
    clip.src_out = src_out;
    clip.name = base;

    canvas::core::Clip aclip;
    aclip.media = found->id;
    aclip.tl_in = frame;
    aclip.src_in = 0;
    aclip.src_out = clip.src_out;
    aclip.name = base + " Audio";

    // The linked audio half still needs a home: ensure at least one audio
    // channel exists (A1), created below the video section when there is none.
    ensure_tracks_at(canvas::core::Track::Kind::Audio, 0);

    auto cmd = canvas::core::place_linked_clip(project_->sequence, static_cast<std::size_t>(lane), 0,
                                           std::move(clip), std::move(aclip), mode, found->fps);
    if (!cmd) return false;
    qWarning() << "[edit] PLACE media=" << media_id << "v_track=" << lane << "at=" << frame
               << "mode=" << static_cast<int>(mode)
               << "path=" << QString::fromStdString(found->path);
    undo_.record(std::move(cmd));
    has_unsaved_changes_ = true;
    refresh_timeline();
    push_snapshot();
    return true;
}

void MainWindow::refresh_media_pool() {
    if (!media_pool_) return;
    // The Dual-Viewer source preview can point at a pooled entry that just got
    // removed (or the whole pool rebuilt): drop the stale preview so it can't
    // read an orphaned snapshot anymore.
    if (src_preview_.has_media() &&
        std::find_if(project_->media.begin(), project_->media.end(),
                     [&](const canvas::core::MediaEntry& m) {
                         return m.path == src_preview_.media_path();
                     }) == project_->media.end()) {
        clear_source_preview();
    }
    media_pool_->clear();
    const QString bin = current_bin_;
    for (std::size_t i = 0; i < project_->media.size(); ++i) {
        const auto& m = project_->media[i];
        if (bin != QString::fromStdString(m.bin)) continue;
        auto* item = new QListWidgetItem;
        item->setData(kPoolMediaIndexRole, static_cast<qlonglong>(i));
        item->setText(QFileInfo(QString::fromStdString(m.path)).completeBaseName());
        item->setTextAlignment(Qt::AlignHCenter | Qt::AlignBottom);
        item->setSizeHint(QSize(124, 110));
        QFont f = item->font();
        f.setPointSizeF(8);
        item->setFont(f);
        item->setForeground(canvas::gui::tokens().ink);

        const bool is_video = m.width > 0 && m.height > 0;
        item->setData(kPoolIsVideoRole, is_video);
        item->setData(kPoolHasAudioRole, m.has_audio);
        item->setData(kPoolResolutionRole,
                      is_video
                          ? QStringLiteral("%1×%2 · %3")
                                .arg(m.width)
                                .arg(m.height)
                                .arg(QString::number(m.fps, 'g', 3))
                          : QString());
        if (m.fps > 0.0 && m.total_frames > 0) {
            const double secs = static_cast<double>(m.total_frames) / m.fps;
            const int64_t total = static_cast<int64_t>(std::llround(secs));
            const int64_t h = total / 3600, mn = (total % 3600) / 60, s = total % 60;
            item->setData(kPoolDurationRole,
                          QStringLiteral("%1:%2:%3")
                              .arg(h, 2, 10, QLatin1Char('0'))
                              .arg(mn, 2, 10, QLatin1Char('0'))
                              .arg(s, 2, 10, QLatin1Char('0')));
        }
        media_pool_->addItem(item);

        if (m.width <= 0 && m.height <= 0) {
            // Audio-only media: paint its spectrum (waveform) as the pool
            // preview so the pool shows the sound rather than a video frame.
            // Video-bearing files request an actual frame below so the pool
            // shows the picture, not a spectrum.
            thumbnails_.request_waveform(static_cast<uint64_t>(i), m.path, 240, 136, 0.0f, 1.0f);
            continue;
        }
        if (m.total_frames <= 0) continue;
        ThumbRequest req;
        req.id = static_cast<uint64_t>(i);
        req.path = m.path;
        req.frame = std::max<int64_t>(0, std::min<int64_t>(m.total_frames / 2, m.total_frames - 1));
        req.target_width = 240;
        req.max_height = 136;
        thumbnails_.request(req);
        if (m.has_audio) {
            // Video+audio media get a hybrid tile: the frame top + this
            // audio-spectrum strip bottom, composed by the tile delegate.
            thumbnails_.request_waveform(static_cast<uint64_t>(i), m.path, 116, 24, 0.0f, 1.0f);
        }
    }

    std::unordered_map<canvas::core::MediaId, canvas::gui::MediaMeta> paths;
    for (const auto& m : project_->media)
        paths[m.id] = canvas::gui::MediaMeta{m.path, m.total_frames, m.fps};
    if (timeline_) timeline_->set_media_paths(std::move(paths));

    std::unordered_map<canvas::core::MediaId, canvas::gui::MiniMediaMeta> strip_paths;
    for (const auto& m : project_->media)
        strip_paths[m.id] = canvas::gui::MiniMediaMeta{m.path, m.total_frames, m.fps};
    if (color_mini_strip_) color_mini_strip_->set_media_paths(std::move(strip_paths));
}

void MainWindow::delete_selected_media() {
    if (!project_ || !media_pool_) return;
    const QList<QListWidgetItem*> items = media_pool_->selectedItems();
    if (items.isEmpty()) return;

    std::vector<int> indices;
    std::vector<canvas::core::MediaId> ids;
    for (const QListWidgetItem* item : items) {
        const QVariant v = item->data(Qt::UserRole);
        if (!v.isValid()) continue;
        const int idx = static_cast<int>(v.toLongLong());
        if (idx < 0 || static_cast<std::size_t>(idx) >= project_->media.size()) continue;
        indices.push_back(idx);
        ids.push_back(project_->media[idx].id);
    }
    if (indices.empty()) return;

    // Clips on the timeline referencing a removed media go with it, and any
    // linked mate (the audio half of a video+audio pair on a partner track) is
    // pulled along so no half is left stranded on the timeline.
    std::vector<canvas::core::ClipId> clips;
    const auto collect = [&](std::vector<canvas::core::Track>& tracks, bool linked_pass) {
        for (const auto& t : tracks) {
            for (const auto& c : t.clips) {
                if (linked_pass) {
                    if (c.is_linked() &&
                        std::find(clips.begin(), clips.end(), c.linked_id) != clips.end())
                        clips.push_back(c.id);
                } else if (std::find(ids.begin(), ids.end(), c.media) != ids.end()) {
                    clips.push_back(c.id);
                }
            }
        }
    };
    collect(project_->sequence.video_tracks, /*linked_pass=*/false);
    collect(project_->sequence.audio_tracks, /*linked_pass=*/false);
    collect(project_->sequence.video_tracks, /*linked_pass=*/true);
    collect(project_->sequence.audio_tracks, /*linked_pass=*/true);

    const auto erase_clips = [&](std::vector<canvas::core::Track>& tracks) {
        for (auto& t : tracks) {
            t.clips.erase(
                std::remove_if(t.clips.begin(), t.clips.end(),
                               [&](const canvas::core::Clip& c) {
                                   return std::find(clips.begin(), clips.end(), c.id) != clips.end();
                               }),
                t.clips.end());
        }
    };
    erase_clips(project_->sequence.video_tracks);
    erase_clips(project_->sequence.audio_tracks);

    // Pool entries are indexed by list position; erase high-to-low so the
    // earlier indices stay valid.
    std::sort(indices.begin(), indices.end());
    for (auto it = indices.rbegin(); it != indices.rend(); ++it)
        project_->media.erase(project_->media.begin() + *it);

    refresh_media_pool();
    refresh_timeline();
    push_snapshot(current_frame_);
    has_unsaved_changes_ = true;
    status_->showMessage(
        tr("Removed %1 media item(s) from the pool").arg(indices.size()), 4000);
}

void MainWindow::delete_selected_media_and_clips() {
    delete_selected_media();
    if (timeline_ && !timeline_->selected_clip_ids().empty()) {
        delete_selected_clip(/*ripple=*/true);
    }
}

void MainWindow::refresh_bin_tree() {
    if (!bin_tree_ || !project_) return;
    bin_tree_->blockSignals(true);
    bin_tree_->clear();
    QIcon bin_icon = icon("folder");
    // The Master bin's icon is the studio's VHS tape (the legacy Event-Horizon
    // mascot kept in the redesign). It is raster art, so it loads raw rather
    // than through the tinted SVG engine.
    QIcon master_icon(QStringLiteral(":/icons/vhs.svg"));

    auto count_in_bin = [this](const QString& bin) {
        return static_cast<int>(std::count_if(
            project_->media.begin(), project_->media.end(),
            [&](const canvas::core::MediaEntry& m) {
                return QString::fromStdString(m.bin) == bin;
            }));
    };
    auto make_count_font = [] {
        QFont f;
        f.setFamily(canvas::gui::tokens().font_mono);
        f.setPointSizeF(9);
        return f;
    };

    auto* master = new QTreeWidgetItem(bin_tree_, QStringList{tr("Master")});
    master->setData(0, Qt::UserRole, QString());
    master->setIcon(0, master_icon);
    master->setFirstColumnSpanned(true);
    for (const auto& b : project_->bins) {
        QString name = QString::fromStdString(b);
        auto* item = new QTreeWidgetItem(bin_tree_, QStringList{name});
        item->setData(0, Qt::UserRole, name);
        item->setFlags(item->flags() | Qt::ItemIsEditable);
        item->setIcon(0, bin_icon);
        item->setText(1, QString::number(count_in_bin(name)));
        item->setFont(1, make_count_font());
    }
    bin_tree_->expandAll();
    // Re-select the current bin, defaulting to Master.
    QTreeWidgetItem* to_select = master;
    for (int i = 0; i < bin_tree_->topLevelItemCount(); ++i) {
        if (bin_tree_->topLevelItem(i)->data(0, Qt::UserRole).toString() == current_bin_) {
            to_select = bin_tree_->topLevelItem(i);
            break;
        }
    }
    bin_tree_->setCurrentItem(to_select);
    bin_tree_->blockSignals(false);
}

void MainWindow::set_current_bin(const QString& bin_name) {
    if (project_ && current_bin_ != bin_name) {
        // Rebuild the persisted bin list to match the visible tree (Master excluded).
        project_->bins.clear();
        for (int i = 1; i < bin_tree_->topLevelItemCount(); ++i)
            project_->bins.push_back(bin_tree_->topLevelItem(i)->data(0, Qt::UserRole).toString().toStdString());
    }
    current_bin_ = bin_name;
    refresh_media_pool();
}

void MainWindow::on_import_media() {
    if (!project_) return;
    const QStringList paths = QFileDialog::getOpenFileNames(
        this, tr("Import Media"), QString(),
        tr("Media Files (*.mp4 *.mov *.mkv *.mxf *.avi *.webm *.ts *.m2ts "
           "*.png *.jpg *.jpeg "
           "*.mp3 *.mp2 *.flac *.wav *.aac *.m4a *.m4b *.ogg *.oga *.opus "
           "*.wma *.aiff *.aifc *.mka *.ac3 *.eac3 *.dts *.ape *.wv *.tta "
           "*.tak *.au *.snd *.spx *.caf);;All Files (*)"));
    if (paths.isEmpty()) return;
    import_media_paths(paths);
}

int MainWindow::import_media_paths(const QStringList& paths) {
    if (!project_) return 0;
    int imported = 0;
    for (const QString& path : paths) {
        std::string error;
        // Always-on per-import probe timing: open/stream-probe cost, plus the
        // media's own dims/fps/duration. A slow probe blocks the UI thread here,
        // so climbing probe_ms across imports explains import stalls (it used to
        // feel random whether a big file would hang).
        const auto probe_t0 = std::chrono::steady_clock::now();
        canvas::core::VideoDecoder probe;
        if (probe.open(path.toStdString(), &error)) {
            const double probe_ms = std::chrono::duration<double, std::milli>(
                                        std::chrono::steady_clock::now() - probe_t0).count();
            canvas::core::MediaEntry entry;
            entry.id = static_cast<canvas::core::MediaId>(project_->media.size());
            entry.path = path.toStdString();
            entry.fps = probe.frame_rate();
            entry.width = probe.width();
            entry.height = probe.height();
            entry.total_frames = probe.total_frames();
            entry.bin = current_bin_.toStdString();
            entry.has_audio = probe.has_audio();

            // A fresh (untitled) project starts at the default 30fps; adopt the
            // first video's own rate so a 60fps clip plays at 60fps cadence
            // instead of a halved 30fps scrub/present. Guarded to the untouched
            // sequence (default fps, no media, no placed clips) — a project whose
            // user picked a rate or already has content keeps it.
            if (project_->sequence.fps == 30.0 && project_->media.empty() &&
                project_->sequence.video_tracks.empty() &&
                project_->sequence.audio_tracks.empty()) {
                const double first_fps = probe.frame_rate();
                if (first_fps > 0.0) {
                    project_->sequence.fps = first_fps;
                    qWarning().nospace() << "[seq] adopted fps="
                                         << QString::number(first_fps, 'f', 3)
                                         << " from first media: " << path;
                    if (timeline_) timeline_->set_fps(first_fps);
                    update_fps_label();
                    // Re-anchor the playback worker on the adopted fps (it caches
                    // the last snapshot's rate for pacing).
                    push_snapshot(current_frame_);
                }
            }

            project_->media.push_back(entry);
            controller_.add_media(entry);

            qWarning().nospace()
                << "[import] VIDEO probe_ms=" << QString::number(probe_ms, 'f', 0)
                << " dims=" << entry.width << "x" << entry.height
                << " fps=" << QString::number(entry.fps, 'f', 3)
                << " frames=" << entry.total_frames
                << " audio=" << (entry.has_audio ? "yes" : "no")
                << " path=" << path;

            status_->showMessage(tr("Imported: %1 (%2x%3, %4fps)")
                                     .arg(path)
                                     .arg(entry.width)
                                     .arg(entry.height)
                                     .arg(entry.fps, 0, 'g', 3),
                                 5000);
            ++imported;
            continue;
        }

        // No video stream: the file may be a pure audio track (mp3/flac/wav/
        // ogg/opus/m4a/...). Probe it as audio-only media so it can be placed
        // directly on an audio lane. Audio has no natural frame rate, so it is
        // measured at the sequence's frame rate (every consumer of MediaEntry.fps
        // then agrees on where frames sit in time).
        canvas::core::AudioDecoder aprobe;
        if (aprobe.open(path.toStdString()) && aprobe.has_audio()) {
            const double probe_ms = std::chrono::duration<double, std::milli>(
                                        std::chrono::steady_clock::now() - probe_t0).count();
            const double seq_fps = project_->sequence.fps > 0.0 ? project_->sequence.fps : 30.0;
            const double secs = aprobe.duration_seconds();
            canvas::core::MediaEntry entry;
            entry.id = static_cast<canvas::core::MediaId>(project_->media.size());
            entry.path = path.toStdString();
            entry.fps = seq_fps;
            entry.width = 0;
            entry.height = 0;
            entry.total_frames =
                secs > 0.0 ? static_cast<int64_t>(std::llround(secs * seq_fps)) : 0;
            entry.bin = current_bin_.toStdString();
            entry.has_audio = true;
            project_->media.push_back(entry);
            controller_.add_media(entry);

            qWarning().nospace()
                << "[import] AUDIO probe_ms=" << QString::number(probe_ms, 'f', 0)
                << " secs=" << QString::number(secs, 'f', 1)
                << " rate=" << aprobe.source_sample_rate()
                << " path=" << path;

            status_->showMessage(tr("Imported: %1 (audio, %2s, %3 Hz)")
                                     .arg(path)
                                     .arg(secs, 0, 'f', 1)
                                     .arg(aprobe.source_sample_rate()),
                                 5000);
            ++imported;
            continue;
        }

        status_->showMessage(tr("Failed to open media: %1").arg(QString::fromStdString(error)), 8000);
    }
    refresh_media_pool();
    has_unsaved_changes_ = true;
    return imported;
}

void MainWindow::on_new_project() { new_untitled_project(); refresh_bin_tree(); refresh_media_pool(); refresh_timeline(); push_snapshot(0); }

void MainWindow::on_open_project() {
    const QString path = QFileDialog::getOpenFileName(this, tr("Open Project"), QString(),
                                                      tr("Nova Canvas Project (*.ehproj);;All Files (*)"));
    if (path.isEmpty()) return;
    open_file(path);
}

bool MainWindow::save_project_to(const QString& path) {
    // Persist the deliver context with the timeline: panel settings + a
    // snapshot of the current queue (staged, finished cards, failures).
    if (deliver_settings_) project_->deliver_settings = deliver_settings_->settings();
    const auto jobs = render_queue_.jobs();
    project_->render_jobs.clear();
    project_->render_jobs.reserve(jobs.size());
    for (const auto& j : jobs)
        project_->render_jobs.push_back(canvas::core::render_job_snapshot(j));

    std::string error;
    if (!canvas::core::save_project(*project_, path.toStdString(), &error)) {
        status_->showMessage(tr("Save failed: %1").arg(QString::fromStdString(error)), 8000);
        return false;
    }
    has_unsaved_changes_ = false;
    project_path_ = path;
    remember_recent_project(path);
    status_->showMessage(tr("Saved project to %1").arg(path), 5000);
    return true;
}

void MainWindow::on_save_project() {
    if (!project_) return;
    if (project_path_.isEmpty()) {
        // Untitled project (or a project without a file yet): Save acts as
        // Save As so there's something to write to.
        on_save_project_as();
        return;
    }
    save_project_to(project_path_);
}

void MainWindow::on_save_project_as() {
    if (!project_) return;
    QString suggested = project_path_;
    if (suggested.isEmpty()) {
        QString name = QString::fromStdString(project_->name).trimmed();
        if (name.isEmpty()) name = tr("Untitled");
        suggested = name + QStringLiteral(".ehproj");
    }
    QString path = QFileDialog::getSaveFileName(this, tr("Save Project As"), suggested,
                                                tr("Nova Canvas Project (*.ehproj);;All Files (*)"));
    if (path.isEmpty()) return;
    if (!path.endsWith(QStringLiteral(".ehproj"))) path += QStringLiteral(".ehproj");
    save_project_to(path);
}

void MainWindow::open_file(const QString& path) {
    std::unique_ptr<canvas::core::Project> loaded = std::make_unique<canvas::core::Project>();
    std::string error;
    if (canvas::core::load_project(*loaded, path.toStdString(), &error)) {
        project_ = std::move(loaded);
        undo_.clear();
        has_unsaved_changes_ = false;
        project_path_ = path;
        remember_recent_project(path);
        current_bin_.clear();
        // The Color page's mini-strip caches a raw pointer into the project's
        // sequence; re-point it now that the old Project (and its Sequence) is
        // gone, so a repaint on an already-open Color page can't dereference
        // the freed object.
        if (color_mini_strip_) color_mini_strip_->set_sequence(&project_->sequence);
        media_pool_->clear();
        for (const auto& m : project_->media) {
            controller_.add_media(m);
        }
        refresh_bin_tree();
        refresh_media_pool();
        refresh_timeline();
        push_snapshot(0);
        // Reinstate deliver context: panel settings + the render queue exactly
        // as it was saved (finished cards keep their completion time).
        if (deliver_settings_) deliver_settings_->set_settings(project_->deliver_settings);
        std::vector<canvas::core::RenderJob> restored;
        restored.reserve(project_->render_jobs.size());
        for (const auto& sn : project_->render_jobs)
            restored.push_back(canvas::core::render_job_from_snapshot(sn));
        render_queue_.restore(restored);
        render_queue_.set_active_project(
            std::make_shared<const canvas::core::Project>(*project_), {});
        reflect_render_queue();
        setWindowTitle(tr("Nova Canvas Studio — %1").arg(QString::fromStdString(project_->name)));
        status_->showMessage(tr("Opened project %1").arg(path), 5000);
    } else {
        status_->showMessage(tr("Failed to open project: %1").arg(QString::fromStdString(error)), 8000);
    }
}

QStringList MainWindow::recent_projects() const {
    return QSettings().value(QStringLiteral("recentProjects")).toStringList();
}

void MainWindow::remember_recent_project(const QString& path) {
    if (path.isEmpty()) return;
    const QFileInfo info(path);
    if (!info.exists()) return;

    QStringList recent = recent_projects();
    recent.removeAll(path);
    recent.prepend(info.absoluteFilePath());
    while (recent.size() > 10) recent.removeLast();

    QSettings settings;
    settings.setValue(QStringLiteral("recentProjects"), recent);
    rebuild_recent_menu();
}

void MainWindow::rebuild_recent_menu() {
    if (!open_recent_menu_) return;
    open_recent_menu_->clear();

    const QStringList recent = recent_projects();
    open_recent_menu_->setEnabled(!recent.isEmpty());
    if (recent.isEmpty()) {
        auto* empty = open_recent_menu_->addAction(tr("No recent projects"));
        empty->setEnabled(false);
        return;
    }
    for (const QString& path : recent) {
        const QFileInfo info(path);
        const QString label = info.exists()
            ? tr("%1 — %2").arg(info.completeBaseName(), info.absolutePath())
            : tr("%1 (missing)").arg(path);
        open_recent_menu_->addAction(label)->setData(path);
    }
}

void MainWindow::on_open_recent_file(QAction* action) {
    if (!action) return;
    const QString path = action->data().toString();
    if (path.isEmpty()) return;
    if (!QFileInfo::exists(path)) {
        status_->showMessage(tr("Recent project no longer exists: %1").arg(path), 8000);
        // Drop the stale entry from the list.
        QStringList recent = recent_projects();
        recent.removeAll(path);
        QSettings().setValue(QStringLiteral("recentProjects"), recent);
        rebuild_recent_menu();
        return;
    }
    open_file(path);
}

void MainWindow::on_undo() {
    if (!project_ || !undo_.can_undo()) return;
    undo_.undo(project_->sequence);
    has_unsaved_changes_ = true;
    refresh_timeline();
    refresh_media_pool();
    update_inspector_audio_full(*this);
    update_inspector_file(*this);
    update_inspector_visual(*this);
    push_snapshot();
}

void MainWindow::on_redo() {
    if (!project_ || !undo_.can_redo()) return;
    undo_.redo(project_->sequence);
    has_unsaved_changes_ = true;
    refresh_timeline();
    refresh_media_pool();
    update_inspector_audio_full(*this);
    update_inspector_file(*this);
    update_inspector_visual(*this);
    push_snapshot();
}

}  // namespace canvas::gui
