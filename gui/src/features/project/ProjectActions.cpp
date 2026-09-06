#include "UX/MainWindow.hpp"
#include "Logging.hpp"

#include "Widgets/media_pool_widget.hpp"

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
}

void MainWindow::ensure_tracks(std::size_t min_video, std::size_t min_audio) {
    while (project_->sequence.video_tracks.size() < min_video) {
        canvas::core::Track t;
        t.kind = canvas::core::Track::Kind::Video;
        t.name = "V" + std::to_string(project_->sequence.video_tracks.size() + 1);
        project_->sequence.video_tracks.push_back(std::move(t));
    }
    while (project_->sequence.audio_tracks.size() < min_audio) {
        canvas::core::Track t;
        t.kind = canvas::core::Track::Kind::Audio;
        t.name = "A" + std::to_string(project_->sequence.audio_tracks.size() + 1);
        project_->sequence.audio_tracks.push_back(std::move(t));
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

bool MainWindow::place_media_at(canvas::core::MediaId media_id, int64_t frame, canvas::core::Placement mode) {
    if (!project_) return false;
    const canvas::core::MediaEntry* found = nullptr;
    for (const auto& m : project_->media) {
        if (m.id == media_id) { found = &m; break; }
    }
    if (!found) return false;

    ensure_tracks(1, 1);
    const bool audio_only = found->width <= 0 && found->height <= 0;
    const int64_t src_out = found->total_frames > 0
                                ? found->total_frames
                                : 300 * static_cast<int64_t>(found->fps > 0 ? found->fps : 30.0);
    const std::string base = QFileInfo(QString::fromStdString(found->path))
                                 .completeBaseName()
                                 .toStdString();

    // Pure audio media (mp3/flac/wav/...): lands as a single audio clip on A1.
    // Creating a linked video half would put a dead video clip (no decode) on
    // V1, so audio-only media stays audio-only.
    if (audio_only) {
        canvas::core::Clip aclip;
        aclip.media = found->id;
        aclip.tl_in = frame;
        aclip.src_in = 0;
        aclip.src_out = src_out;
        aclip.name = base + " Audio";
        auto cmd = canvas::core::place_clip(project_->sequence, canvas::core::Track::Kind::Audio, 0,
                                        std::move(aclip), mode);
        if (!cmd) return false;
        qWarning() << "[edit] PLACE-AUDIO media=" << media_id << "at=" << frame
                   << "mode=" << static_cast<int>(mode)
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

    auto cmd = canvas::core::place_linked_clip(project_->sequence, 0, 0, std::move(clip),
                                           std::move(aclip), mode);
    if (!cmd) return false;
    qWarning() << "[edit] PLACE media=" << media_id << "at=" << frame
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
    media_pool_->clear();
    const QString bin = current_bin_;
    for (std::size_t i = 0; i < project_->media.size(); ++i) {
        const auto& m = project_->media[i];
        if (bin != QString::fromStdString(m.bin)) continue;
        auto* item = new QListWidgetItem;
        item->setData(Qt::UserRole, static_cast<qlonglong>(i));
        item->setText(QFileInfo(QString::fromStdString(m.path)).completeBaseName());
        item->setTextAlignment(Qt::AlignHCenter | Qt::AlignBottom);
        item->setSizeHint(QSize(120, 96));
        QFont f = item->font();
        f.setPointSizeF(8);
        item->setFont(f);
        item->setForeground(QColor(220, 225, 230));
        media_pool_->addItem(item);

        if (m.width <= 0 && m.height <= 0) {
            // Audio-only media: paint its spectrum (waveform) as the pool
            // preview rather than a video frame (there is none).
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
    }

    std::unordered_map<canvas::core::MediaId, canvas::gui::MediaMeta> paths;
    for (const auto& m : project_->media)
        paths[m.id] = canvas::gui::MediaMeta{m.path, m.total_frames};
    if (timeline_) timeline_->set_media_paths(std::move(paths));
}

void MainWindow::refresh_bin_tree() {
    if (!bin_tree_ || !project_) return;
    bin_tree_->blockSignals(true);
    bin_tree_->clear();
    QIcon bin_icon(QStringLiteral(":/icons/vhs.svg"));
    auto* master = new QTreeWidgetItem(bin_tree_, QStringList{tr("Master")});
    master->setData(0, Qt::UserRole, QString());
    master->setIcon(0, bin_icon);
    for (const auto& b : project_->bins) {
        QString name = QString::fromStdString(b);
        auto* item = new QTreeWidgetItem(bin_tree_, QStringList{name});
        item->setData(0, Qt::UserRole, name);
        item->setFlags(item->flags() | Qt::ItemIsEditable);
        item->setIcon(0, bin_icon);
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
        canvas::core::VideoDecoder probe;
        if (probe.open(path.toStdString(), &error)) {
            canvas::core::MediaEntry entry;
            entry.id = static_cast<canvas::core::MediaId>(project_->media.size());
            entry.path = path.toStdString();
            entry.fps = probe.frame_rate();
            entry.width = probe.width();
            entry.height = probe.height();
            entry.total_frames = probe.total_frames();
            entry.bin = current_bin_.toStdString();
            project_->media.push_back(entry);
            controller_.add_media(entry);

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
            project_->media.push_back(entry);
            controller_.add_media(entry);

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
        media_pool_->clear();
        for (const auto& m : project_->media) {
            controller_.add_media(m);
        }
        refresh_bin_tree();
        refresh_media_pool();
        refresh_timeline();
        push_snapshot(0);
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
    update_inspector_audio();
    update_inspector_visual(*this);
    push_snapshot();
}

void MainWindow::on_redo() {
    if (!project_ || !undo_.can_redo()) return;
    undo_.redo(project_->sequence);
    has_unsaved_changes_ = true;
    refresh_timeline();
    refresh_media_pool();
    update_inspector_audio();
    update_inspector_visual(*this);
    push_snapshot();
}

}  // namespace canvas::gui
