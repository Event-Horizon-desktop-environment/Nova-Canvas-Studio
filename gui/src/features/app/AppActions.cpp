#include "UX/MainWindow.hpp"
#include "Logging.hpp"

#include <QCloseEvent>
#include <QKeyEvent>
#include <QMessageBox>

#include <algorithm>
#include <cstdint>
#include <utility>

namespace canvas::gui {

void MainWindow::keyPressEvent(QKeyEvent* event) {
    const bool ctrl = event->modifiers().testFlag(Qt::ControlModifier);
    const bool shift = event->modifiers().testFlag(Qt::ShiftModifier);
    const bool alt = event->modifiers().testFlag(Qt::AltModifier);

    if (ctrl && event->key() == Qt::Key_S) { on_save_project(); return; }
    if (ctrl && event->key() == Qt::Key_O) { on_open_project(); return; }
    if (ctrl && event->key() == Qt::Key_I) { on_import_media(); return; }
    if (ctrl && event->key() == Qt::Key_N) { on_new_project(); return; }
    if (ctrl && event->key() == Qt::Key_D) { toggle_disable_selected_clip(); return; }
    if (ctrl && event->key() == Qt::Key_T) { toggle_transition_on_selected(); return; }
    if (ctrl && event->key() == Qt::Key_Z) {
        const bool redoing = shift;
        if (redoing && undo_.can_redo())
            qWarning() << "[edit] REDO cmd=" << QString::fromStdString(undo_.next_redo_name());
        else if (!redoing && undo_.can_undo())
            qWarning() << "[edit] UNDO cmd=" << QString::fromStdString(undo_.next_undo_name())
                       << "depth=" << undo_.count();
        bool changed = false;
        if (redoing) changed = undo_.redo(project_->sequence);
        else changed = undo_.undo(project_->sequence);
        if (changed) {
            has_unsaved_changes_ = true;
            refresh_timeline();
            push_snapshot();
        }
        return;
    }

    switch (event->key()) {
    case Qt::Key_Space:
        controller_.toggle_play_pause();
        return;
    case Qt::Key_K:
        controller_.pause();
        return;
    case Qt::Key_J:
        if (alt) break;
        controller_.pause();
        controller_.step(-1);
        return;
    case Qt::Key_L:
        if (alt) break;
        controller_.play();
        return;
    case Qt::Key_Left:
        if (shift) { controller_.pause(); controller_.step(-static_cast<int64_t>(fps_)); }
        else { controller_.pause(); controller_.step(-1); }
        return;
    case Qt::Key_Right:
        if (shift) { controller_.pause(); controller_.step(static_cast<int64_t>(fps_)); }
        else { controller_.pause(); controller_.step(1); }
        return;
    case Qt::Key_Home:
        controller_.pause(); controller_.seek(0);
        return;
    case Qt::Key_End:
        controller_.pause(); controller_.seek(total_frames_ - 1);
        return;
    case Qt::Key_A:
        timeline_->set_tool(TimelineWidget::Tool::Select);
        return;
    case Qt::Key_B:
        timeline_->set_tool(TimelineWidget::Tool::Blade);
        return;
    case Qt::Key_M:
        toggle_bookmark_at_playhead();
        return;
    case Qt::Key_E:
        place_selected_media(canvas::core::Placement::AppendAtEnd);
        return;
    case Qt::Key_Insert:
        if (event->modifiers().testFlag(Qt::KeypadModifier)) break;
        return;
    case Qt::Key_F9:
        place_selected_media(canvas::core::Placement::Insert);
        return;
    case Qt::Key_F10:
        if (shift) break;
        place_selected_media(canvas::core::Placement::Overwrite);
        return;
    case Qt::Key_F12:
        place_selected_media(canvas::core::Placement::PlaceOnTop);
        return;
    case Qt::Key_Delete:
        delete_selected_clip(/*ripple=*/true);
        return;
    case Qt::Key_Backspace:
        delete_selected_clip(/*ripple=*/false);
        return;
    default:
        QMainWindow::keyPressEvent(event);
    }
}

void MainWindow::delete_selected_clip(const bool ripple) {
    if (!project_) return;

    // Clicking a transition bubble selects it; Delete/Backspace (or the Delete
    // menu) then clears that transition instead of deleting a clip.
    if (timeline_ && timeline_->delete_selected_transition()) return;

    std::vector<canvas::core::ClipId> ids;
    if (timeline_ && !timeline_->selected_clip_ids().empty())
        ids = timeline_->selected_clip_ids();
    else if (selected_clip_ != 0)
        ids.push_back(selected_clip_);
    if (ids.empty()) return;

    if (debug_enabled()) {
        QString s;
        for (const auto id : ids)
            s += QString::number(static_cast<quint64>(id)) + QLatin1Char(' ');
        qDebug() << "delete: selected ids ->" << s << "ripple=" << ripple;
    }    // The selected ids may already contain both halves of a linked pair
    // (set_selection expands a selection to include each clip's linked mate).
    // The core delete ops (lift_clip / ripple_delete_clip) remove a clip
    // together with its linked mate on the partner track, so a single op per
    // pair is sufficient. Scheduling both halves would run the delete twice:
    // for Lift the second op travels through the linked-mate machinery and can
    // remove an adjacent clip the user did not intend (e.g. the audio clip to
    // the right of a split pair). For each linked pair we keep one
    // representative so the loop below issues exactly one command per pair.
    const auto find = [&](canvas::core::ClipId id) -> const canvas::core::Clip* {
        for (auto& t : project_->sequence.video_tracks)
            if (const canvas::core::Clip* c = t.clip_with_id(id)) return c;
        for (auto& t : project_->sequence.audio_tracks)
            if (const canvas::core::Clip* c = t.clip_with_id(id)) return c;
        return nullptr;
    };
    std::vector<canvas::core::ClipId> to_delete;
    for (const canvas::core::ClipId id : ids) {
        if (std::find(to_delete.begin(), to_delete.end(), id) != to_delete.end())
            continue;
        const canvas::core::Clip* c = find(id);
        if (!c) continue;
        if (c->is_linked() &&
            std::find(ids.begin(), ids.end(), c->linked_id) != ids.end()) {
            // Both halves of a linked pair are selected: only the
            // numerically-lower id is kept as the pair's representative.
            const canvas::core::ClipId rep = std::min(id, c->linked_id);
            if (std::find(to_delete.begin(), to_delete.end(), rep) == to_delete.end())
                to_delete.push_back(rep);
            continue;
        }
        to_delete.push_back(id);
    }

    if (debug_enabled()) {
        QString s;
        for (const auto id : to_delete)
            s += QString::number(static_cast<quint64>(id)) + QLatin1Char(' ');
        qDebug() << "delete: representatives (one op per linked pair) ->" << s;
    }

    bool any = false;
    for (const canvas::core::ClipId id : to_delete) {
        std::unique_ptr<canvas::core::ICommand> cmd;
        for (std::size_t vi = 0; !cmd && vi < project_->sequence.video_tracks.size(); ++vi) {
            if (project_->sequence.video_tracks[vi].clip_with_id(id))
                cmd = ripple
                    ? canvas::core::ripple_delete_clip(project_->sequence, canvas::core::Track::Kind::Video, vi, id)
                    : canvas::core::lift_clip(project_->sequence, canvas::core::Track::Kind::Video, vi, id);
        }
        for (std::size_t ai = 0; !cmd && ai < project_->sequence.audio_tracks.size(); ++ai) {
            if (project_->sequence.audio_tracks[ai].clip_with_id(id))
                cmd = ripple
                    ? canvas::core::ripple_delete_clip(project_->sequence, canvas::core::Track::Kind::Audio, ai, id)
                    : canvas::core::lift_clip(project_->sequence, canvas::core::Track::Kind::Audio, ai, id);
        }
        if (cmd) {
            qWarning() << "[edit] DELETE" << (ripple ? "ripple" : "lift")
                       << "id=" << static_cast<quint64>(id)
                       << "cmd=" << QString::fromStdString(cmd->name());
            undo_.record(std::move(cmd));
            any = true;
        }
    }

    if (any) {
        has_unsaved_changes_ = true;
        refresh_timeline();
        push_snapshot();
        if (timeline_) timeline_->clear_selection();
    }
    selected_clip_ = 0;
}

void MainWindow::toggle_disable_selected_clip() {
    if (!project_) return;

    std::vector<canvas::core::ClipId> ids;
    if (timeline_ && !timeline_->selected_clip_ids().empty())
        ids = timeline_->selected_clip_ids();
    else if (selected_clip_ != 0)
        ids.push_back(selected_clip_);
    if (ids.empty()) return;

    // The selected ids may include both halves of a linked pair (selection
    // auto-expands to linked mates). set_clip_enabled toggles the mate itself,
    // so keep one representative per pair to apply exactly one command.
    const auto find = [&](canvas::core::ClipId id) -> const canvas::core::Clip* {
        for (auto& t : project_->sequence.video_tracks)
            if (const canvas::core::Clip* c = t.clip_with_id(id)) return c;
        for (auto& t : project_->sequence.audio_tracks)
            if (const canvas::core::Clip* c = t.clip_with_id(id)) return c;
        return nullptr;
    };
    std::vector<canvas::core::ClipId> reps;
    for (const canvas::core::ClipId id : ids) {
        if (std::find(reps.begin(), reps.end(), id) != reps.end())
            continue;
        const canvas::core::Clip* c = find(id);
        if (!c) continue;
        if (c->is_linked() && std::find(ids.begin(), ids.end(), c->linked_id) != ids.end()) {
            const canvas::core::ClipId rep = std::min(id, c->linked_id);
            if (std::find(reps.begin(), reps.end(), rep) == reps.end())
                reps.push_back(rep);
            continue;
        }
        reps.push_back(id);
    }

    // Determine the resulting enabled state from the first representative.
    const canvas::core::Clip* first = find(reps.front());
    if (!first) return;
    const bool enabling = !first->enabled;

    bool any = false;
    for (const canvas::core::ClipId id : reps) {
        std::unique_ptr<canvas::core::ICommand> cmd;
        for (std::size_t vi = 0; !cmd && vi < project_->sequence.video_tracks.size(); ++vi) {
            if (project_->sequence.video_tracks[vi].clip_with_id(id))
                cmd = canvas::core::set_clip_enabled(project_->sequence, canvas::core::Track::Kind::Video, vi, id, enabling);
        }
        for (std::size_t ai = 0; !cmd && ai < project_->sequence.audio_tracks.size(); ++ai) {
            if (project_->sequence.audio_tracks[ai].clip_with_id(id))
                cmd = canvas::core::set_clip_enabled(project_->sequence, canvas::core::Track::Kind::Audio, ai, id, enabling);
        }
        if (cmd) {
            qWarning() << "[edit] SET-ENABLED id=" << static_cast<quint64>(id)
                       << "-> enabled=" << enabling;
            undo_.record(std::move(cmd));
            any = true;
        }
    }

    if (any) {
        has_unsaved_changes_ = true;
        refresh_timeline();
        push_snapshot();
    }
}

void MainWindow::toggle_transition_on_selected() {
    if (!project_) return;

    canvas::core::ClipId id = 0;
    if (timeline_ && !timeline_->selected_clip_ids().empty())
        id = timeline_->selected_clip_ids().front();
    else if (selected_clip_ != 0)
        id = selected_clip_;
    if (id == 0) return;

    const auto find = [&](canvas::core::ClipId cid) -> const canvas::core::Clip* {
        for (auto& t : project_->sequence.video_tracks)
            if (const canvas::core::Clip* c = t.clip_with_id(cid)) return c;
        for (auto& t : project_->sequence.audio_tracks)
            if (const canvas::core::Clip* c = t.clip_with_id(cid)) return c;
        return nullptr;
    };
    const canvas::core::Clip* c = find(id);
    if (!c) return;

    // Toggle a default Cross Dissolve: if the clip already carries one, clear it;
    // otherwise set it (6 frames). Delegates to the same edit commands used by
    // the context menu.
    const bool clearing =
        c->transition_out == canvas::core::TransitionType::CrossDissolve && c->has_transition();
    std::unique_ptr<canvas::core::ICommand> cmd;
    for (std::size_t vi = 0; !cmd && vi < project_->sequence.video_tracks.size(); ++vi) {
        if (project_->sequence.video_tracks[vi].clip_with_id(id))
            cmd = clearing
                ? canvas::core::clear_clip_transition(project_->sequence, canvas::core::Track::Kind::Video, vi, id)
                : canvas::core::set_clip_transition(project_->sequence, canvas::core::Track::Kind::Video, vi,
                                                id, canvas::core::TransitionType::CrossDissolve, 6);
    }
    for (std::size_t ai = 0; !cmd && ai < project_->sequence.audio_tracks.size(); ++ai) {
        if (project_->sequence.audio_tracks[ai].clip_with_id(id))
            cmd = clearing
                ? canvas::core::clear_clip_transition(project_->sequence, canvas::core::Track::Kind::Audio, ai, id)
                : canvas::core::set_clip_transition(project_->sequence, canvas::core::Track::Kind::Audio, ai,
                                                id, canvas::core::TransitionType::CrossDissolve, 6);
    }
    if (cmd) {
        qWarning() << "[edit] TOGGLE-TRANSITION id=" << static_cast<quint64>(id)
                   << "clearing=" << clearing;
        undo_.record(std::move(cmd));
        has_unsaved_changes_ = true;
        refresh_timeline();
        push_snapshot();
    }
}

void MainWindow::toggle_bookmark_at_playhead() {
    if (!project_) return;
    qWarning() << "[edit] BOOKMARK toggle frame=" << current_frame_;
    (void)project_->sequence.toggle_bookmark(current_frame_, "");
    has_unsaved_changes_ = true;
    refresh_timeline();
    push_snapshot();
}

void MainWindow::closeEvent(QCloseEvent* event) {
    if (has_unsaved_changes_) {
        const auto ret = QMessageBox::question(
            this, tr("Unsaved Changes"), tr("The project has unsaved changes. Discard them?"),
            QMessageBox::Discard | QMessageBox::Cancel);
        if (ret != QMessageBox::Discard) {
            event->ignore();
            return;
        }
    }
    event->accept();
}

}  // namespace canvas::gui
