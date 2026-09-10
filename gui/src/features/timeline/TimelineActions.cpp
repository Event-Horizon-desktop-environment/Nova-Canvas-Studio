#include "UX/MainWindow.hpp"
#include "UX/InspectorAudio.hpp"
#include "UX/InspectorFile.hpp"
#include "Logging.hpp"

#include <QDoubleSpinBox>

#include <cstdint>
#include <cmath>
#include <algorithm>
#include <utility>

#include "Widgets/timeline_widget.hpp"
#include "features/timeline/audio_targets.hpp"
#include "canvas/core/timeline/audio_mix.hpp"

namespace canvas::gui {

void MainWindow::activate_color_clip(const canvas::core::ClipId id) {
    if (id == 0) {
        selected_clip_ = 0;
        selected_clip_ids_.clear();
        return;
    }
    selected_clip_ = id;
    selected_clip_ids_ = {id};
}

void MainWindow::connect_timeline() {
    timeline_->set_sequence(&project_->sequence);

    connect(timeline_, &TimelineWidget::playhead_moved, this,
            [this](int64_t frame) {
                // Scrubbing: this fires once on grab then on every move. Mark the
                // drag active so seek_preview uses the fast, non-rewinding preview
                // path even mid-playback (avoids per-move audio pipe churn).
                controller_.begin_scrub();
                // Playback keeps running; previews are cheap and never rewind audio.
                controller_.seek_preview(frame);
            });

    connect(timeline_, &TimelineWidget::playhead_committed, this,
            [this](int64_t frame) {
                // Scrub released: end the drag (single audio re-anchor + crisp
                // full-res frame), superseding any buffered previews.
                controller_.end_scrub();
                controller_.seek(frame);
            });

    connect(timeline_, &TimelineWidget::clip_selected, this,
            [this](const canvas::core::Clip* clip) {
                selected_clip_ = clip ? clip->id : 0;
                // Read AFTER the widget's press handler resolved Ctrl/Shift/
                // plain-click selection for this press (the widget emits this
                // signal from the end of the press handler), so the mixer sees
                // the full current set.
                selected_clip_ids_ = timeline_->selected_clip_ids();
                update_inspector_audio_full(*this);
                update_inspector_file(*this);
            });

    connect(timeline_, &TimelineWidget::clips_range_selected, this,
            [this](std::vector<canvas::core::ClipId> ids) {
                timeline_->set_selection(ids);
                selected_clip_ = ids.empty() ? 0 : ids.front();
                selected_clip_ids_ = std::move(ids);
                update_inspector_audio_full(*this);
                update_inspector_file(*this);
            });

    // Volume-line drag (Phase 5): the live preview (waveform re-scale + audible
    // mix override) rides the Phase-4 preview path; on release the settled dB is
    // committed to every selected audio target, keeping each clip's pan.
    connect(timeline_, &TimelineWidget::volume_line_preview, this,
            [this](float db) { preview_inspector_volume(db); });
    connect(timeline_, &TimelineWidget::volume_line_committed, this,
            [this](float db) {
                if (!project_) return;
                const auto targets =
                    resolve_audio_targets(project_->sequence, selected_clip_ids_);
                bool any = false;
                for (const auto& t : targets) {
                    if (std::abs(static_cast<double>(db) - t.clip.volume_db) < 0.05) continue;
                    auto cmd = canvas::core::set_clip_audio(project_->sequence, t.kind, t.track,
                                                            t.id, db, t.clip.pan);
                    if (!cmd) continue;
                    undo_.record(std::move(cmd));
                    any = true;
                }
                // ALWAYS drop the live drag override and repaint from the model,
                // even when nothing changed (no selected targets / knob already at
                // the value): otherwise the line stays at its last live-drag
                // position while the model holds the pre-drag level and the next
                // redraw "snaps back" — the reported volume-line reset.
                controller_.clear_live_clip_gains();
                refresh_timeline();
                if (!any) return;
                has_unsaved_changes_ = true;
                push_audio_mix_snapshot();
            });

    connect(timeline_, &TimelineWidget::blade_requested, this,
            [this](const canvas::core::Clip* clip, int64_t frame) {
                if (!clip || !project_) return;
                for (std::size_t vi = 0; vi < project_->sequence.video_tracks.size(); ++vi) {
                    if (project_->sequence.video_tracks[vi].clip_with_id(clip->id)) {
                        auto cmd = canvas::core::blade_linked_at(project_->sequence, canvas::core::Track::Kind::Video,
                                                             vi, frame);
                        if (cmd) {
                            undo_.record(std::move(cmd));
                            qDebug() << "[edit] BLADE v_track=" << vi << "clip=" << clip->id
                                       << "at=" << frame;
                            has_unsaved_changes_ = true;
                            refresh_timeline();
                            push_snapshot();
                        }
                        return;
                    }
                }
                for (std::size_t ai = 0; ai < project_->sequence.audio_tracks.size(); ++ai) {
                    if (project_->sequence.audio_tracks[ai].clip_with_id(clip->id)) {
                        auto cmd = canvas::core::blade_linked_at(project_->sequence, canvas::core::Track::Kind::Audio,
                                                             ai, frame);
                        if (cmd) {
                            undo_.record(std::move(cmd));
                            qDebug() << "[edit] BLADE a_track=" << ai << "clip=" << clip->id
                                       << "at=" << frame;
                            has_unsaved_changes_ = true;
                            refresh_timeline();
                            push_snapshot();
                        }
                        return;
                    }
                }
            });

    connect(timeline_, &TimelineWidget::clip_moved, this,
            [this](const canvas::core::Clip* clip, int64_t new_tl_in, canvas::core::Track::Kind dst_kind,
                   int dst_track) {
                if (!clip || !project_) return;
                std::size_t dst = static_cast<std::size_t>(dst_track);
                for (std::size_t vi = 0; vi < project_->sequence.video_tracks.size(); ++vi) {
                    if (project_->sequence.video_tracks[vi].clip_with_id(clip->id)) {
                        auto cmd = canvas::core::move_clip(
                            project_->sequence, canvas::core::Track::Kind::Video, vi, clip->id,
                            dst_kind, dst, new_tl_in);
                        if (cmd) {
                            undo_.record(std::move(cmd));
                            qDebug() << "[edit] MOVE v_track=" << vi << "clip=" << clip->id
                                       << "-> tl_in=" << new_tl_in << "dst_kind="
                                       << (dst_kind == canvas::core::Track::Kind::Video ? "V" : "A")
                                       << "dst_track=" << dst;
                            has_unsaved_changes_ = true;
                            refresh_timeline();
                            push_snapshot();
                        }
                        return;
                    }
                }
                for (std::size_t ai = 0; ai < project_->sequence.audio_tracks.size(); ++ai) {
                    if (project_->sequence.audio_tracks[ai].clip_with_id(clip->id)) {
                        auto cmd = canvas::core::move_clip(
                            project_->sequence, canvas::core::Track::Kind::Audio, ai, clip->id,
                            dst_kind, dst, new_tl_in);
                        if (cmd) {
                            undo_.record(std::move(cmd));
                            qDebug() << "[edit] MOVE a_track=" << ai << "clip=" << clip->id
                                       << "-> tl_in=" << new_tl_in << "dst_kind="
                                       << (dst_kind == canvas::core::Track::Kind::Video ? "V" : "A")
                                       << "dst_track=" << dst;
                            has_unsaved_changes_ = true;
                            refresh_timeline();
                            push_snapshot();
                        }
                        return;
                    }
                }
            });

    connect(timeline_, &TimelineWidget::clips_moved, this,
            [this](const std::vector<TimelineWidget::MovedClip>& clips) {
                if (!project_ || clips.empty()) return;

                // Resolve each moved clip's CURRENT source (kind, track) in the
                // sequence, then commit as ONE atomic batch op. Linked A/V pairs
                // are already expanded into the payload; the batch op auto-moves
                // the mate, so keep only the numerically-lower id per linked pair
                // (mirrors the delete path's de-dup).
                std::vector<canvas::core::ClipId> reps;
                bool any = false;
                for (const auto& mv : clips) {
                    if (std::find(reps.begin(), reps.end(), mv.id) != reps.end()) continue;
                    const canvas::core::Clip* c = nullptr;
                    for (const auto& t : project_->sequence.video_tracks)
                        if ((c = t.clip_with_id(mv.id))) break;
                    if (!c)
                        for (const auto& t : project_->sequence.audio_tracks)
                            if ((c = t.clip_with_id(mv.id))) break;
                    if (!c) continue;
                    if (c->is_linked() &&
                        std::find_if(clips.begin(), clips.end(),
                                     [&](const TimelineWidget::MovedClip& o) {
                                         return o.id == c->linked_id;
                                     }) != clips.end()) {
                        const canvas::core::ClipId rep = std::min(mv.id, c->linked_id);
                        if (std::find(reps.begin(), reps.end(), rep) == reps.end())
                            reps.push_back(rep);
                        continue;
                    }
                    reps.push_back(mv.id);
                }

                std::vector<canvas::core::BatchMove> batch;
                for (const canvas::core::ClipId id : reps) {
                    const TimelineWidget::MovedClip* entry = nullptr;
                    for (const auto& mv : clips)
                        if (mv.id == id) { entry = &mv; break; }
                    if (!entry) continue;
                    batch.push_back(canvas::core::BatchMove{
                        entry->id, entry->kind,
                        static_cast<std::size_t>(entry->track_index), entry->new_tl_in});
                }
                // Atomic batch commit: the whole dragged group is placed in one
                // undo command, so the clips keep their exact preview size and
                // relative spacing even when they pass over each other's old
                // positions or station the destination.
                if (!batch.empty()) {
                    auto cmd = canvas::core::move_clips_batch(project_->sequence, batch);
                    if (cmd) {
                        undo_.record(std::move(cmd));
                        has_unsaved_changes_ = true;
                        any = true;
                    }
                }
                if (any) {
                    refresh_timeline();
                    push_snapshot();
                }
            });

    connect(timeline_, &TimelineWidget::clip_trimmed, this,
            [this](const canvas::core::Clip* clip, TimelineWidget::TrimEdge edge,
                   int64_t new_frame) {
                if (!clip || !project_) return;

                // The core trim op needs the clip's media duration to bound the
                // source window. MediaRegistry lookup by MediaId; missing media
                // (0) disables source extension entirely.
                int64_t media_frames = 0;
                for (const auto& m : project_->media) {
                    if (m.id == clip->media) {
                        media_frames = m.total_frames;
                        break;
                    }
                }

                const auto commit = [&](canvas::core::Track::Kind kind, std::size_t track) {
                    std::unique_ptr<canvas::core::ICommand> cmd =
                        edge == TimelineWidget::TrimEdge::Head
                            ? canvas::core::trim_clip_head(project_->sequence, kind, track,
                                                           clip->id, new_frame, media_frames)
                            : canvas::core::trim_clip_tail(project_->sequence, kind, track,
                                                           clip->id, new_frame, media_frames);
                    // A clamped no-op (e.g. regrowing a head already at media
                    // start) must still restore the widget preview to model truth.
                    if (!cmd) {
                        refresh_timeline();
                        return;
                    }
                    undo_.record(std::move(cmd));
                    qDebug() << "[edit] TRIM"
                               << (edge == TimelineWidget::TrimEdge::Head ? "head" : "tail")
                               << "kind=" << (kind == canvas::core::Track::Kind::Video ? "V" : "A")
                               << "track=" << track << "clip=" << clip->id
                               << "-> " << new_frame;
                    has_unsaved_changes_ = true;
                    refresh_timeline();
                    push_snapshot();
                };

                for (std::size_t vi = 0; vi < project_->sequence.video_tracks.size(); ++vi)
                    if (project_->sequence.video_tracks[vi].clip_with_id(clip->id)) {
                        commit(canvas::core::Track::Kind::Video, vi);
                        return;
                    }
                for (std::size_t ai = 0; ai < project_->sequence.audio_tracks.size(); ++ai)
                    if (project_->sequence.audio_tracks[ai].clip_with_id(clip->id)) {
                        commit(canvas::core::Track::Kind::Audio, ai);
                        return;
                    }
            });

    connect(timeline_, &TimelineWidget::new_upper_track_requested, this,
            [this](canvas::core::ClipId clip_id, int64_t tl_in) {
                if (!project_) return;
                auto cmd = canvas::core::create_top_track_move(project_->sequence, clip_id, tl_in);
                if (cmd) {
                    undo_.record(std::move(cmd));
                    qDebug() << "[edit] AUTO-TRACK clip=" << clip_id << "tl_in=" << tl_in;
                    has_unsaved_changes_ = true;
                    // Synchronous rebuild; the widget re-acquires its clip items
                    // by id right after this signal returns.
                    refresh_timeline();
                    push_snapshot();
                }
            });

    connect(timeline_, &TimelineWidget::unlink_requested, this,
            [this](canvas::core::Track::Kind kind, int track_index, canvas::core::ClipId id) {
                if (!project_) return;
                auto cmd = canvas::core::unlink_clip(
                    project_->sequence, kind, static_cast<std::size_t>(track_index), id);
                if (cmd) {
                    undo_.record(std::move(cmd));
                    qDebug() << "[edit] UNLINK kind="
                               << (kind == canvas::core::Track::Kind::Video ? "V" : "A")
                               << "track=" << track_index << "clip=" << id;
                    has_unsaved_changes_ = true;
                    refresh_timeline();
                    push_snapshot();
                }
            });

    connect(timeline_, &TimelineWidget::link_requested, this,
            [this](canvas::core::Track::Kind kind, int track_index, canvas::core::ClipId id) {
                if (!project_) return;
                auto cmd = canvas::core::link_clip(
                    project_->sequence, kind, static_cast<std::size_t>(track_index), id);
                if (cmd) {
                    undo_.record(std::move(cmd));
                    qDebug() << "[edit] LINK kind="
                               << (kind == canvas::core::Track::Kind::Video ? "V" : "A")
                               << "track=" << track_index << "clip=" << id;
                    has_unsaved_changes_ = true;
                    refresh_timeline();
                    push_snapshot();
                }
            });

    connect(timeline_, &TimelineWidget::transition_requested, this,
            [this](const canvas::core::Clip* clip, canvas::core::TransitionType type, int64_t duration) {
                if (!clip || !project_) return;
                if (debug_enabled())
                    qDebug() << "timeline: transition_requested clip" << clip->id
                             << "type" << static_cast<int>(type) << "dur" << duration;
                const auto apply = [&](canvas::core::Track::Kind kind, std::size_t tidx) {
                    auto cmd = canvas::core::set_clip_transition(
                        project_->sequence, kind, tidx, clip->id, type, duration);
                    if (cmd) {
                        undo_.record(std::move(cmd));
                        qDebug() << "[edit] SET-TRANSITION kind="
                                   << (kind == canvas::core::Track::Kind::Video ? "V" : "A")
                                   << "track=" << tidx << "clip=" << clip->id
                                   << "type=" << static_cast<int>(type) << "dur=" << duration;
                        has_unsaved_changes_ = true;
                        refresh_timeline();
                        push_snapshot();
                    }
                };
                const auto& seq = project_->sequence;
                for (std::size_t vi = 0; vi < seq.video_tracks.size(); ++vi) {
                    if (seq.video_tracks[vi].clip_with_id(clip->id)) {
                        apply(canvas::core::Track::Kind::Video, vi);
                        return;
                    }
                }
                for (std::size_t ai = 0; ai < seq.audio_tracks.size(); ++ai) {
                    if (seq.audio_tracks[ai].clip_with_id(clip->id)) {
                        apply(canvas::core::Track::Kind::Audio, ai);
                        return;
                    }
                }
            });

    connect(timeline_, &TimelineWidget::clear_transition_requested, this,
            [this](const canvas::core::Clip* clip) {
                if (!clip || !project_) return;
                if (debug_enabled())
                    qDebug() << "timeline: clear_transition_requested clip" << clip->id;
                const auto apply = [&](canvas::core::Track::Kind kind, std::size_t tidx) {
                    auto cmd =
                        canvas::core::clear_clip_transition(project_->sequence, kind, tidx, clip->id);
                    if (cmd) {
                        undo_.record(std::move(cmd));
                        qDebug() << "[edit] CLEAR-TRANSITION kind="
                                   << (kind == canvas::core::Track::Kind::Video ? "V" : "A")
                                   << "track=" << tidx << "clip=" << clip->id;
                        has_unsaved_changes_ = true;
                        refresh_timeline();
                        push_snapshot();
                    }
                };
                const auto& seq = project_->sequence;
                for (std::size_t vi = 0; vi < seq.video_tracks.size(); ++vi) {
                    if (seq.video_tracks[vi].clip_with_id(clip->id)) {
                        apply(canvas::core::Track::Kind::Video, vi);
                        return;
                    }
                }
                for (std::size_t ai = 0; ai < seq.audio_tracks.size(); ++ai) {
                    if (seq.audio_tracks[ai].clip_with_id(clip->id)) {
                        apply(canvas::core::Track::Kind::Audio, ai);
                        return;
                    }
                }
            });

    connect(timeline_, &TimelineWidget::transition_in_requested, this,
            [this](const canvas::core::Clip* clip, canvas::core::TransitionType type, int64_t duration) {
                if (!clip || !project_) return;
                if (debug_enabled())
                    qDebug() << "timeline: transition_in_requested clip" << clip->id
                             << "type" << static_cast<int>(type) << "dur" << duration;
                const auto apply = [&](canvas::core::Track::Kind kind, std::size_t tidx) {
                    auto cmd = canvas::core::set_clip_transition_in(
                        project_->sequence, kind, tidx, clip->id, type, duration);
                    if (cmd) {
                        undo_.record(std::move(cmd));
                        qDebug() << "[edit] SET-TRANSITION-IN kind="
                                   << (kind == canvas::core::Track::Kind::Video ? "V" : "A")
                                   << "track=" << tidx << "clip=" << clip->id
                                   << "type=" << static_cast<int>(type) << "dur=" << duration;
                        has_unsaved_changes_ = true;
                        refresh_timeline();
                        push_snapshot();
                    }
                };
                const auto& seq = project_->sequence;
                for (std::size_t vi = 0; vi < seq.video_tracks.size(); ++vi) {
                    if (seq.video_tracks[vi].clip_with_id(clip->id)) {
                        apply(canvas::core::Track::Kind::Video, vi);
                        return;
                    }
                }
                for (std::size_t ai = 0; ai < seq.audio_tracks.size(); ++ai) {
                    if (seq.audio_tracks[ai].clip_with_id(clip->id)) {
                        apply(canvas::core::Track::Kind::Audio, ai);
                        return;
                    }
                }
            });

    connect(timeline_, &TimelineWidget::clear_transition_in_requested, this,
            [this](const canvas::core::Clip* clip) {
                if (!clip || !project_) return;
                if (debug_enabled())
                    qDebug() << "timeline: clear_transition_in_requested clip" << clip->id;
                const auto apply = [&](canvas::core::Track::Kind kind, std::size_t tidx) {
                    auto cmd = canvas::core::clear_clip_transition_in(
                        project_->sequence, kind, tidx, clip->id);
                    if (cmd) {
                        undo_.record(std::move(cmd));
                        qDebug() << "[edit] CLEAR-TRANSITION-IN kind="
                                   << (kind == canvas::core::Track::Kind::Video ? "V" : "A")
                                   << "track=" << tidx << "clip=" << clip->id;
                        has_unsaved_changes_ = true;
                        refresh_timeline();
                        push_snapshot();
                    }
                };
                const auto& seq = project_->sequence;
                for (std::size_t vi = 0; vi < seq.video_tracks.size(); ++vi) {
                    if (seq.video_tracks[vi].clip_with_id(clip->id)) {
                        apply(canvas::core::Track::Kind::Video, vi);
                        return;
                    }
                }
                for (std::size_t ai = 0; ai < seq.audio_tracks.size(); ++ai) {
                    if (seq.audio_tracks[ai].clip_with_id(clip->id)) {
                        apply(canvas::core::Track::Kind::Audio, ai);
                        return;
                    }
                }
            });

    connect(timeline_, &TimelineWidget::delete_transition_requested, this,
            [this](const canvas::core::Clip* a, const canvas::core::Clip* b, bool in_edge) {
                if (!a || !project_) return;
                if (debug_enabled())
                    qDebug() << "timeline: delete_transition_requested a" << a->id << "b"
                             << (b ? static_cast<quint64>(b->id) : 0) << "in_edge" << in_edge;
                auto& seq = project_->sequence;
                bool changed = false;
                const auto clear_edge = [&](const canvas::core::Clip* clip, bool in) {
                    if (!clip) return;
                    for (std::size_t vi = 0; vi < seq.video_tracks.size(); ++vi) {
                        if (seq.video_tracks[vi].clip_with_id(clip->id)) {
                            auto cmd =
                                in ? canvas::core::clear_clip_transition_in(
                                         seq, canvas::core::Track::Kind::Video, vi, clip->id)
                                   : canvas::core::clear_clip_transition(
                                         seq, canvas::core::Track::Kind::Video, vi, clip->id);
                            if (cmd) { undo_.record(std::move(cmd)); changed = true; }
                            return;
                        }
                    }
                    for (std::size_t ai = 0; ai < seq.audio_tracks.size(); ++ai) {
                        if (seq.audio_tracks[ai].clip_with_id(clip->id)) {
                            auto cmd =
                                in ? canvas::core::clear_clip_transition_in(
                                         seq, canvas::core::Track::Kind::Audio, ai, clip->id)
                                   : canvas::core::clear_clip_transition(
                                         seq, canvas::core::Track::Kind::Audio, ai, clip->id);
                            if (cmd) { undo_.record(std::move(cmd)); changed = true; }
                            return;
                        }
                    }
                };
                if (b) {
                    // Cut bubble: clear whichever edge actually carries the
                    // transition (A's OUT and/or B's IN), plus linked mates.
                    if (a->has_transition_out()) clear_edge(a, false);
                    if (b->has_transition_in()) clear_edge(b, true);
                } else {
                    // Single-clip edge bubble on `a`.
                    if (in_edge) {
                        if (a->has_transition_in()) clear_edge(a, true);
                    } else {
                        if (a->has_transition_out()) clear_edge(a, false);
                    }
                }
                if (changed) {
                    qDebug() << "[edit] DELETE-TRANSITION (bubble)";
                    has_unsaved_changes_ = true;
                    refresh_timeline();
                    push_snapshot();
                }
            });

    connect(timeline_, &TimelineWidget::transition_resized, this,
            [this](const canvas::core::Clip* clip, int64_t duration) {
                if (!clip || !project_ || duration < 1) return;
                if (debug_enabled())
                    qDebug() << "timeline: transition_resized clip" << clip->id << "dur" << duration
                             << "current_type" << static_cast<int>(clip->transition_out);
                // Keep the clip's existing transition type; only the duration
                // changed (via the hover editor). If the clip had none yet,
                // default to a cross-dissolve.
                canvas::core::TransitionType type = clip->transition_out;
                if (type == canvas::core::TransitionType::None)
                    type = canvas::core::TransitionType::CrossDissolve;
                const auto apply = [&](canvas::core::Track::Kind kind, std::size_t tidx) {
                    auto cmd = canvas::core::set_clip_transition(
                        project_->sequence, kind, tidx, clip->id, type, duration);
                    if (cmd) {
                        undo_.record(std::move(cmd));
                        qDebug() << "[edit] TRANSITION-RESIZE kind="
                                   << (kind == canvas::core::Track::Kind::Video ? "V" : "A")
                                   << "track=" << tidx << "clip=" << clip->id
                                   << "dur=" << duration;
                        has_unsaved_changes_ = true;
                        refresh_timeline();
                        push_snapshot();
                    }
                };
                const auto& seq = project_->sequence;
                for (std::size_t vi = 0; vi < seq.video_tracks.size(); ++vi) {
                    if (seq.video_tracks[vi].clip_with_id(clip->id)) {
                        apply(canvas::core::Track::Kind::Video, vi);
                        return;
                    }
                }
                for (std::size_t ai = 0; ai < seq.audio_tracks.size(); ++ai) {
                    if (seq.audio_tracks[ai].clip_with_id(clip->id)) {
                        apply(canvas::core::Track::Kind::Audio, ai);
                        return;
                    }
                }
            });

    connect(timeline_, &TimelineWidget::transition_in_resized, this,
            [this](const canvas::core::Clip* clip, int64_t duration) {
                if (!clip || !project_ || duration < 1) return;
                if (debug_enabled())
                    qDebug() << "timeline: transition_in_resized clip" << clip->id << "dur" << duration
                             << "current_type" << static_cast<int>(clip->transition_in);
                // Keep the clip's existing IN transition type; only the duration
                // changed (via the hover editor). If the clip had none yet,
                // default to a fade-in.
                canvas::core::TransitionType type = clip->transition_in;
                if (type == canvas::core::TransitionType::None)
                    type = canvas::core::TransitionType::FadeIn;
                const auto apply = [&](canvas::core::Track::Kind kind, std::size_t tidx) {
                    auto cmd = canvas::core::set_clip_transition_in(
                        project_->sequence, kind, tidx, clip->id, type, duration);
                    if (cmd) {
                        undo_.record(std::move(cmd));
                        qDebug() << "[edit] TRANSITION-IN-RESIZE kind="
                                   << (kind == canvas::core::Track::Kind::Video ? "V" : "A")
                                   << "track=" << tidx << "clip=" << clip->id
                                   << "dur=" << duration;
                        has_unsaved_changes_ = true;
                        refresh_timeline();
                        push_snapshot();
                    }
                };
                const auto& seq = project_->sequence;
                for (std::size_t vi = 0; vi < seq.video_tracks.size(); ++vi) {
                    if (seq.video_tracks[vi].clip_with_id(clip->id)) {
                        apply(canvas::core::Track::Kind::Video, vi);
                        return;
                    }
                }
                for (std::size_t ai = 0; ai < seq.audio_tracks.size(); ++ai) {
                    if (seq.audio_tracks[ai].clip_with_id(clip->id)) {
                        apply(canvas::core::Track::Kind::Audio, ai);
                        return;
                    }
                }
            });

    connect(timeline_, &TimelineWidget::delete_through_edit_requested, this,
            [this](const canvas::core::Clip* clip) {
                if (!clip || !project_) return;
                if (debug_enabled())
                    qDebug() << "timeline: delete_through_edit_requested out clip" << clip->id;
                const auto& seq = project_->sequence;
                for (std::size_t vi = 0; vi < seq.video_tracks.size(); ++vi) {
                    if (seq.video_tracks[vi].clip_with_id(clip->id)) {
                        auto cmd = canvas::core::delete_through_edit(
                            project_->sequence, canvas::core::Track::Kind::Video, vi, clip->id);
                        if (cmd) {
                            undo_.record(std::move(cmd));
                            qDebug() << "[edit] THROUGH-EDIT v_track=" << vi
                                       << "out_clip=" << clip->id;
                            has_unsaved_changes_ = true;
                            refresh_timeline();
                            push_snapshot();
                        } else if (debug_enabled()) {
                            qDebug() << "timeline: delete_through_edit rejected (not a through edit)";
                        }
                        return;
                    }
                }
                for (std::size_t ai = 0; ai < seq.audio_tracks.size(); ++ai) {
                    if (seq.audio_tracks[ai].clip_with_id(clip->id)) {
                        auto cmd = canvas::core::delete_through_edit(
                            project_->sequence, canvas::core::Track::Kind::Audio, ai, clip->id);
                        if (cmd) {
                            undo_.record(std::move(cmd));
                            qDebug() << "[edit] THROUGH-EDIT a_track=" << ai
                                       << "out_clip=" << clip->id;
                            has_unsaved_changes_ = true;
                            refresh_timeline();
                            push_snapshot();
                        } else if (debug_enabled()) {
                            qDebug() << "timeline: delete_through_edit rejected (not a through edit)";
                        }
                        return;
                    }
                }
            });

    connect(timeline_, &TimelineWidget::add_track_requested, this,
            [this](canvas::core::Track::Kind kind) {
                if (!project_) return;
                auto& tracks = kind == canvas::core::Track::Kind::Video
                                   ? project_->sequence.video_tracks
                                   : project_->sequence.audio_tracks;
                canvas::core::Track t;
                t.kind = kind;
                t.name = (kind == canvas::core::Track::Kind::Video ? "V" : "A") +
                         std::to_string(tracks.size() + 1);
                tracks.push_back(std::move(t));
                has_unsaved_changes_ = true;
                refresh_timeline();
                qDebug() << "[edit] ADD-TRACK kind="
                           << (kind == canvas::core::Track::Kind::Video ? "V" : "A")
                           << "index=" << (tracks.size() - 1);
                push_snapshot();
            });

    connect(timeline_, &TimelineWidget::delete_track_requested, this,
            [this](canvas::core::Track::Kind kind, int track_index) {
                if (!project_ || track_index < 0) return;
                auto& tracks = kind == canvas::core::Track::Kind::Video
                                   ? project_->sequence.video_tracks
                                   : project_->sequence.audio_tracks;
                const std::size_t idx = static_cast<std::size_t>(track_index);
                if (idx >= tracks.size()) return;
                tracks.erase(tracks.begin() + static_cast<std::ptrdiff_t>(idx));
                has_unsaved_changes_ = true;
                refresh_timeline();
                qDebug() << "[edit] DEL-TRACK kind="
                           << (kind == canvas::core::Track::Kind::Video ? "V" : "A")
                           << "index=" << track_index << "now=" << tracks.size();
                push_snapshot();
            });

    // Track-header M/S/L toggles apply the inverse of the shown state as a
    // single undoable edit, so Undo restores the previous mixing/lock flags.
    const auto toggle_track_flag =
        [this](canvas::core::Track::Kind kind, int track_index, bool on,
               auto make_cmd) {
            if (!project_ || track_index < 0) return;
            auto cmd = make_cmd(project_->sequence, kind,
                                static_cast<std::size_t>(track_index), on);
            if (!cmd) return;
            undo_.record(std::move(cmd));
            has_unsaved_changes_ = true;
            refresh_timeline();
            push_snapshot();
        };
    connect(timeline_, &TimelineWidget::track_mute_toggled, this,
            [toggle_track_flag, this](canvas::core::Track::Kind kind, int idx, bool on) {
                toggle_track_flag(kind, idx, on,
                                  canvas::core::set_track_muted);
            });
    connect(timeline_, &TimelineWidget::track_solo_toggled, this,
            [toggle_track_flag, this](canvas::core::Track::Kind kind, int idx, bool on) {
                toggle_track_flag(kind, idx, on,
                                  canvas::core::set_track_solo);
            });
    connect(timeline_, &TimelineWidget::track_lock_toggled, this,
            [toggle_track_flag, this](canvas::core::Track::Kind kind, int idx, bool on) {
                toggle_track_flag(kind, idx, on,
                                  canvas::core::set_track_locked);
            });
}

bool MainWindow::find_selected_clip(canvas::core::Track::Kind& out_kind, std::size_t& out_index,
                                    canvas::core::Clip& out_clip) const {
    if (!project_ || selected_clip_ == 0) return false;
    const canvas::core::Sequence& seq = project_->sequence;
    for (std::size_t i = 0; i < seq.video_tracks.size(); ++i) {
        for (const auto& c : seq.video_tracks[i].clips) {
            if (c.id == selected_clip_) {
                out_kind = canvas::core::Track::Kind::Video;
                out_index = i;
                out_clip = c;
                return true;
            }
        }
    }
    for (std::size_t i = 0; i < seq.audio_tracks.size(); ++i) {
        for (const auto& c : seq.audio_tracks[i].clips) {
            if (c.id == selected_clip_) {
                out_kind = canvas::core::Track::Kind::Audio;
                out_index = i;
                out_clip = c;
                return true;
            }
        }
    }
    return false;
}

bool MainWindow::find_audio_target(canvas::core::Track::Kind& out_kind, std::size_t& out_index,
                                   canvas::core::Clip& out_clip) const {
    if (!project_ || selected_clip_ == 0) return false;
    const canvas::core::Sequence& seq = project_->sequence;
    for (std::size_t i = 0; i < seq.audio_tracks.size(); ++i) {
        for (const auto& c : seq.audio_tracks[i].clips) {
            if (c.id == selected_clip_) {
                out_kind = canvas::core::Track::Kind::Audio;
                out_index = i;
                out_clip = c;
                return true;
            }
        }
    }
    for (std::size_t i = 0; i < seq.video_tracks.size(); ++i) {
        for (const auto& c : seq.video_tracks[i].clips) {
            if (c.id == selected_clip_) {
                if (c.linked_id == 0) return false;
                for (std::size_t a = 0; a < seq.audio_tracks.size(); ++a) {
                    for (const auto& ac : seq.audio_tracks[a].clips) {
                        if (ac.id == c.linked_id) {
                            out_kind = canvas::core::Track::Kind::Audio;
                            out_index = a;
                            out_clip = ac;
                            return true;
                        }
                    }
                }
                return false;
            }
        }
    }
    return false;
}

void MainWindow::remove_all_transitions() {
    if (!project_) return;
    auto& seq = project_->sequence;

    const auto snapshot_all =
        [](canvas::core::Sequence& s) -> std::vector<canvas::core::TrackSnapshot> {
        std::vector<canvas::core::TrackSnapshot> out;
        const auto collect = [&out](std::vector<canvas::core::Track>& tracks,
                                    canvas::core::Track::Kind kind) {
            for (std::size_t i = 0; i < tracks.size(); ++i) {
                const auto& t = tracks[i];
                canvas::core::TrackSnapshot snap;
                snap.kind = kind;
                snap.index = i;
                snap.locked = t.locked;
                snap.muted = t.muted;
                snap.solo = t.solo;
                snap.gain_db = t.gain_db;
                snap.clips = t.clips;
                out.push_back(std::move(snap));
            }
        };
        collect(s.video_tracks, canvas::core::Track::Kind::Video);
        collect(s.audio_tracks, canvas::core::Track::Kind::Audio);
        return out;
    };

    std::vector<canvas::core::TrackSnapshot> before = snapshot_all(seq);

    std::size_t cleared = 0;
    const auto clear_track =
        [&cleared](canvas::core::Sequence& s, std::vector<canvas::core::Track>& tracks,
                   canvas::core::Track::Kind kind) {
            for (std::size_t ti = 0; ti < tracks.size(); ++ti) {
                const std::vector<canvas::core::Clip> clips = tracks[ti].clips;
                for (const auto& clip : clips) {
                    if (clip.has_transition_out()) {
                        if (canvas::core::clear_clip_transition(s, kind, ti, clip.id))
                            ++cleared;
                    }
                    if (clip.has_transition_in()) {
                        if (canvas::core::clear_clip_transition_in(s, kind, ti, clip.id))
                            ++cleared;
                    }
                }
            }
        };
    clear_track(seq, seq.video_tracks, canvas::core::Track::Kind::Video);
    clear_track(seq, seq.audio_tracks, canvas::core::Track::Kind::Audio);

    if (cleared == 0) return;

    std::vector<canvas::core::TrackSnapshot> after = snapshot_all(seq);
    undo_.record(std::make_unique<canvas::core::EditCommand>(
        "Remove All Transitions", std::move(before), std::move(after)));
    has_unsaved_changes_ = true;
    refresh_timeline();
    push_snapshot();
    qDebug() << "[edit] REMOVE-ALL-TRANSITIONS cleared=" << cleared;
}

void MainWindow::update_inspector_audio() {
    if (!project_ || !inspector_audio_volume_ || !inspector_audio_pan_) return;
    canvas::core::Track::Kind kind;
    std::size_t index = 0;
    canvas::core::Clip clip;
    if (!find_audio_target(kind, index, clip)) return;
    // A pending live-drag override belongs to the previous selection; drop it
    // before rebinding so a released-unchanged knob can't keep re-mixing stale.
    controller_.clear_live_clip_gains();
    inspector_audio_volume_->setValue(clip.volume_db);
    inspector_audio_pan_->setValue(clip.pan);
}

void MainWindow::apply_inspector_audio() {
    if (!project_ || !inspector_audio_volume_ || !inspector_audio_pan_) return;
    const auto targets = resolve_audio_targets(project_->sequence, selected_clip_ids_);
    if (targets.empty()) return;
    // Clamp the Inspector's wide (±100 dB) slider into the AUDIO LAW band on
    // commit. The out-of-law tail (-100..-61, +25..+100) renders identically to
    // the band edges anyway (db_to_gain floors/heels them), so storing nought
    // outside the band removes the "dragged down to -100, no audio at all"
    // surprise while the wide slider keeps 0 dB dead-center.
    const float vol = canvas::core::audio_mix::normalize_volume_db(
        static_cast<float>(inspector_audio_volume_->value()));
    const float pan = static_cast<float>(inspector_audio_pan_->value());
    bool any = false;
    for (const auto& t : targets) {
        if (vol == t.clip.volume_db && pan == t.clip.pan) continue;
        auto cmd = canvas::core::set_clip_audio(project_->sequence, t.kind, t.track, t.id, vol, pan);
        if (!cmd) continue;
        undo_.record(std::move(cmd));
        any = true;
    }
    if (!any) return;
    // The committed edits supersede the live-drag override the mix was using.
    controller_.clear_live_clip_gains();
    has_unsaved_changes_ = true;
    refresh_timeline();
    push_audio_mix_snapshot();
    if (targets.size() == 1)
        qDebug() << "[edit] CLIP-AUDIO kind="
                   << (targets.front().kind == canvas::core::Track::Kind::Video ? "V" : "A")
                   << "track=" << targets.front().track << "clip=" << targets.front().id
                   << "vol_db=" << vol << "pan=" << pan;
    else
        qDebug() << "[edit] CLIP-AUDIO-BATCH clips=" << targets.size()
                   << "vol_db=" << vol << "pan=" << pan;
}

void MainWindow::preview_inspector_volume(float vol_db) {
    if (!project_ || !timeline_) return;
    const auto targets = resolve_audio_targets(project_->sequence, selected_clip_ids_);
    if (targets.empty()) return;
    for (const auto& t : targets) {
        if (std::abs(vol_db - t.clip.volume_db) < 0.05f) {
            // Knob is at the model value: make sure no stale drag override
            // lingers for this clip.
            controller_.clear_live_clip_gain(t.id);
            continue;
        }
        // Re-mix the audible playback at the knob's gain (live volume override);
        // the committed edit still arrives from apply_inspector_audio() on drag
        // release. The timeline's waveform/spread is a CONTENT readout and is
        // intentionally NOT rescaled with volume (that flattened it into a line).
        controller_.set_live_clip_gain(t.id, vol_db);
    }
}

}  // namespace canvas::gui
