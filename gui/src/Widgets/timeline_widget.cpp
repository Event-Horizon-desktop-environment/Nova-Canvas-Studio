#include "Widgets/timeline_widget.hpp"
#include "Logging.hpp"
#include "UX/theme.hpp"

#include <QColor>
#include <QContextMenuEvent>
#include <QMenu>
#include <QOpenGLWidget>
#include <QSurfaceFormat>
#include <QPainter>
#include <QScrollBar>
#include <QKeyEvent>
#include <QWheelEvent>
#include <QGraphicsItem>
#include <QGraphicsRectItem>

#include <algorithm>
#include <cmath>
#include <utility>

namespace canvas::gui {

TimelineWidget::TimelineWidget(QWidget* parent) : QGraphicsView(parent) {
    setScene(&scene_);
    // Render the entire timeline scene on the GPU via an OpenGL viewport.
    // Enable 4x multisampling on the viewport so the vector paths (clip and
    // transition-bubble rounded corners) render smooth, not aliased.
    auto* gl_viewport = new QOpenGLWidget(this);
    QSurfaceFormat fmt = gl_viewport->format();
    fmt.setSamples(4);
    gl_viewport->setFormat(fmt);
    setViewport(gl_viewport);
    setRenderHint(QPainter::Antialiasing, true);
    setDragMode(QGraphicsView::NoDrag);
    setMouseTracking(true);
    setTransformationAnchor(QGraphicsView::AnchorUnderMouse);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOn);
    setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOn);
    setAlignment(Qt::AlignLeft | Qt::AlignTop);
    setAcceptDrops(true);
    scene_.setBackgroundBrush(QColor(0x11, 0x13, 0x1A));
}

void TimelineWidget::set_sequence(const canvas::core::Sequence* sequence) {
    sequence_ = sequence;
    rebuild_timeline();
    update_playhead_position(playhead_frame_);
}

void TimelineWidget::set_fps(double fps) {
    fps_ = fps;
    rebuild_timeline();
}

void TimelineWidget::set_playhead_position(int64_t frame) {
    const int64_t dur = sequence_ ? std::max<int64_t>(sequence_->duration_frames(), 1) : 1;
    playhead_frame_ = std::clamp<int64_t>(frame, 0, dur);
    update_playhead_position(playhead_frame_);
}

void TimelineWidget::set_tool(Tool tool) {
    current_tool_ = tool;
    setCursor(Qt::ArrowCursor);
    if (tool != Tool::Blade) hide_blade_preview();
}

void TimelineWidget::set_snap_enabled(bool enabled) { snap_enabled_ = enabled; }

void TimelineWidget::set_frames_per_pixel(double fpp) {
    frames_per_pixel_ = std::clamp(fpp, kMinFramesPerPixel, kMaxFramesPerPixel);
    rebuild_timeline();
    update_minimap_viewport();
}

int64_t TimelineWidget::frame_at_x(int x) const {
    const QPointF scene_p = mapToScene(QPoint(x, 0));
    const double scene_x = scene_p.x();
    return static_cast<int64_t>(std::floor((scene_x - kSceneMargin - kTrackHeaderWidth) * frames_per_pixel_));
}

void TimelineWidget::zoom_fit() {
    if (!sequence_) return;
    const int64_t dur = std::max<int64_t>(sequence_->duration_frames(), 1);
    const int view_w = std::max(viewport()->width() - kTrackHeaderWidth - 40, 1);
    set_frames_per_pixel(static_cast<double>(dur) / view_w);
}

void TimelineWidget::set_zoom_percent(double percent) {
    set_frames_per_pixel(kDefaultFramesPerPixel /
                         std::clamp(percent, kZoomMinPercent, kZoomMaxPercent) * 100.0);
}

void TimelineWidget::zoom_in() { set_zoom_percent(zoom_percent() * 1.2); }

void TimelineWidget::zoom_out() { set_zoom_percent(zoom_percent() / 1.2); }

double TimelineWidget::tracks_origin_y() const {
    // pan_down_room_ is the small "sink room" below the parked seat of the
    // channels: it offsets the track area down the scene so grab-and-follow
    // panning can dip the rows briefly, while the parked scroll value
    // (pan_down_room_ + top pad) is the up limit that seats them under the ruler
    // strip. The room is fixed and modest (kPanDownRoomMin), so the timeline
    // never opens with a huge empty band under the last track.
    return static_cast<double>(kRulerHeight + kMinimapHeight + kSceneMargin)
           + kTimecodeBarHeight + pan_down_room_ + track_v_pad_top_;
}

double TimelineWidget::track_height(int track_index, int v_count) const {
    if (track_index < v_count) {
        return (track_index >= 0 && track_index < static_cast<int>(video_track_heights_.size()))
                   ? video_track_heights_[track_index]
                   : kDefaultTrackHeight;
    }
    const int ai = track_index - v_count;
    return (ai >= 0 && ai < static_cast<int>(audio_track_heights_.size()))
               ? audio_track_heights_[ai]
               : kDefaultTrackHeight;
}

double TimelineWidget::track_top(int track_index, int v_count) const {
    // Rows stack from the top: video flats descend v-1..0 (Vn on top of V1),
    // then audio flats ascend v..total-1 (A1 on top of A2). Walking in screen
    // order lets each row's top depend on the heights of the rows above it. The
    // fixed Video/Audio divider band sits between the two sections in the slot
    // a single row gap used to occupy, so the audio section starts that much
    // lower.
    double y = tracks_origin_y();
    if (track_index < v_count) {
        for (int f = v_count - 1; f > track_index; --f) y += track_height(f, v_count) + kTrackGap;
        return y;
    }
    for (int f = v_count - 1; f >= 0; --f) y += track_height(f, v_count) + kTrackGap;
    if (v_count > 0) y += kSectionDividerHeight;
    for (int f = v_count; f < track_index; ++f) y += track_height(f, v_count) + kTrackGap;
    return y;
}

double TimelineWidget::tracks_content_height(int v_count, int a_count) const {
    double h = 0.0;
    for (int f = 0; f < v_count; ++f) h += track_height(f, v_count) + kTrackGap;
    for (int f = 0; f < a_count; ++f) h += track_height(v_count + f, v_count) + kTrackGap;
    h += (v_count > 0 && a_count > 0) ? kSectionDividerHeight : 0.0;
    // tracks_origin_y() sits below kSceneMargin; add the top margin back so the
    // returned height is measured from the scene's own top edge. The trailing
    // kTrackGap + track_v_pad_bottom_ leave a matching empty strip below the last
    // row.
    return (tracks_origin_y() - static_cast<double>(kSceneMargin)) + h + track_v_pad_bottom_;
}

double TimelineWidget::tracks_stack_top() const { return tracks_origin_y(); }

double TimelineWidget::tracks_stack_bottom(int v_count, int a_count) const {
    const int band = (v_count > 0 && a_count > 0) ? 1 : 0;
    const int total_elems = v_count + a_count + band;
    if (total_elems <= 0) return tracks_stack_top();
    const int last_flat = flat_of_screen_row(total_elems - 1, v_count);
    return track_top(last_flat, v_count) + track_height(last_flat, v_count);
}

double TimelineWidget::edge_y(int edge, int v_count, int a_count) const {
    if (edge <= 0) return tracks_stack_top();
    const int band = (v_count > 0 && a_count > 0) ? 1 : 0;
    const int total_elems = v_count + a_count + band;
    if (edge >= total_elems) return tracks_stack_bottom(v_count, a_count);
    // Walk the screen-order elements (video rows, divider band, audio rows)
    // accumulating boundary Y positions; the divider band is a fixed band that
    // replaces the ordinary row gap at the video/audio boundary.
    double y = tracks_stack_top();
    int e = edge;
    for (int f = v_count - 1; f >= 0; --f) {
        y += track_height(f, v_count);
        if (--e == 0) return y;
        y += kTrackGap;
    }
    if (band) {
        y += kSectionDividerHeight;
        if (--e == 0) return y;
    }
    for (int f = v_count; f < v_count + a_count; ++f) {
        y += track_height(f, v_count);
        if (--e == 0) return y;
        y += kTrackGap;
    }
    return y;
}

bool TimelineWidget::in_section_divider_band(double scene_y, int v_count, int a_count) const {
    if (!(v_count > 0 && a_count > 0)) return false;
    return scene_y > edge_y(v_count, v_count, a_count) &&
           scene_y < edge_y(v_count + 1, v_count, a_count);
}

int TimelineWidget::header_resize_target(double scene_y, int v_count, int a_count) const {
    // No clip on any track yet: the rows are hidden, so no divider is live.
    if (!sequence_ || !has_timeline_content()) return -1;
    const int band = (v_count > 0 && a_count > 0) ? 1 : 0;
    const int total_elems = v_count + a_count + band;
    // The tolerance is wide enough to cover the kTrackGap between rows plus a
    // little of each adjacent row's header, so users can aim at the divider
    // comfortably either side of it.
    for (int edge = 0; edge <= total_elems; ++edge) {
        if (std::abs(scene_y - edge_y(edge, v_count, a_count)) <= kResizeGrabHalf) return edge;
    }
    return -1;
}

void TimelineWidget::sync_track_heights() {
    const int vn = sequence_ ? static_cast<int>(sequence_->video_tracks.size()) : 0;
    const int an = sequence_ ? static_cast<int>(sequence_->audio_tracks.size()) : 0;
    if (static_cast<int>(video_track_heights_.size()) != vn)
        video_track_heights_.assign(static_cast<std::size_t>(vn), kDefaultTrackHeight);
    if (static_cast<int>(audio_track_heights_.size()) != an)
        audio_track_heights_.assign(static_cast<std::size_t>(an), kDefaultTrackHeight);
}

void TimelineWidget::set_track_height(int flat_track, int v_count, const double height) {
    const double clamped = std::clamp(height, kMinTrackHeight, kMaxTrackHeight);
    if (flat_track < v_count && flat_track >= 0 &&
        flat_track < static_cast<int>(video_track_heights_.size()))
        video_track_heights_[flat_track] = clamped;
    else if (flat_track >= v_count && flat_track - v_count < static_cast<int>(audio_track_heights_.size()))
        audio_track_heights_[flat_track - v_count] = clamped;
}

void TimelineWidget::set_media_paths(std::unordered_map<canvas::core::MediaId, MediaMeta> paths) {
    media_paths_ = std::move(paths);
}

void TimelineWidget::wheelEvent(QWheelEvent* event) {
    if (event->modifiers().testFlag(Qt::ControlModifier)) {
        const double factor_pct = event->angleDelta().y() > 0 ? 1.15 : 1.0 / 1.15;
        set_zoom_percent(zoom_percent() * factor_pct);
        event->accept();
        return;
    }
    QGraphicsView::wheelEvent(event);
}

void TimelineWidget::keyPressEvent(QKeyEvent* event) {
    if (event->key() == Qt::Key_Shift && event->modifiers().testFlag(Qt::ShiftModifier)) {
        if (frames_per_pixel() > 0) {
            // Shift+Z handled at MainWindow level
        }
    }
    // Delete/Backspace on a selected transition bubble clears that transition
    // instead of deleting a clip.
    if ((event->key() == Qt::Key_Delete || event->key() == Qt::Key_Backspace) &&
        delete_selected_transition()) {
        event->accept();
        return;
    }
    QGraphicsView::keyPressEvent(event);
}

void TimelineWidget::keyReleaseEvent(QKeyEvent* event) { QGraphicsView::keyReleaseEvent(event); }

void TimelineWidget::resizeEvent(QResizeEvent* event) {
    QGraphicsView::resizeEvent(event);
    rebuild_timeline();
    update_playhead_position(playhead_frame_);
    update_minimap_viewport();
}

void TimelineWidget::scrollContentsBy(int dx, int dy) {
    QGraphicsView::scrollContentsBy(dx, dy);
    // Keep the ruler + minimap pinned to the top of the viewport: the group's
    // children live in original scene coords, so shifting the group to the
    // current vertical scroll value makes it sit exactly at the viewport top no
    // matter how far the track content pans underneath it.
    if (top_pinned_ && verticalScrollBar())
        top_pinned_->setPos(0.0, static_cast<double>(verticalScrollBar()->value()));
    update_minimap_viewport();
}

void TimelineWidget::contextMenuEvent(QContextMenuEvent* event) {
    if (!sequence_) {
        QGraphicsView::contextMenuEvent(event);
        return;
    }
    const QPointF scene_pos = mapToScene(event->pos());
    QMenu menu(this);

    // Right-clicking directly on an edit point (cut) between two clips shows the
    // cut context menu: delete-through-edit + preset cross-dissolve lengths.
    const CutTarget cut = cut_at_scene_pos(scene_pos);
    if (cut.is_cut()) {
        emit clip_selected(cut.a);
        const bool valid_through =
            cut.a->media == cut.b->media && cut.b->src_in == cut.a->src_out;
        QAction* delete_through = menu.addAction(tr("Delete Through Edit"));
        delete_through->setEnabled(valid_through);
        if (!valid_through) delete_through->setToolTip(tr("Clips do not share continuous source"));
        menu.addSeparator();
        std::vector<QAction*> add_actions;
        for (const int dur : {14, 30, 60, 120}) {
            QAction* act = menu.addAction(tr("Add %1 frame Cross Dissolve").arg(dur));
            act->setIcon(icon("transition"));
            act->setData(dur);
            add_actions.push_back(act);
        }
        QAction* chosen = menu.exec(event->globalPos());
        if (chosen == delete_through) {
            if (debug_enabled())
                qDebug() << "timeline: cut menu Delete Through Edit @"
                         << "out clip" << cut.a->id << "media" << cut.a->media
                         << "-> in clip" << cut.b->id << "valid_through=" << valid_through;
            emit delete_through_edit_requested(cut.a);
        } else {
            auto it = std::find(add_actions.begin(), add_actions.end(), chosen);
            if (it != add_actions.end()) {
                const int dur = chosen->data().toInt();
                if (debug_enabled())
                    qDebug() << "timeline: cut menu Add" << dur << "frame Cross Dissolve on out clip"
                             << cut.a->id;
                emit transition_requested(cut.a, canvas::core::TransitionType::CrossDissolve, dur);
            }
        }
        event->accept();
        return;
    }

    // Right-clicking directly on a single-clip edge (no neighbouring cut) shows a
    // menu identical in spirit to the cut menu: preset frame lengths for the edge
    // transition. A Start edge (clip head) targets the IN (fade-in) transition; an
    // End edge (clip tail, no incoming clip) targets the OUT (fade-out) transition.
    if (cut.valid() && !cut.is_cut()) {
        emit clip_selected(cut.a);
        bool is_in = cut.edge == Edge::Start;
        const QString what = tr(is_in ? "Fade In" : "Fade Out");
        std::vector<QAction*> add_actions;
        for (const int dur : {14, 30, 60, 120}) {
            QAction* act = menu.addAction(
                tr("Add %1 frame %2").arg(dur).arg(what));
            act->setIcon(icon(is_in ? "transition_in" : "transition_out"));
            act->setData(dur);
            add_actions.push_back(act);
        }
        QAction* chosen = menu.exec(event->globalPos());
        auto it = std::find(add_actions.begin(), add_actions.end(), chosen);
        if (it != add_actions.end()) {
            const int dur = chosen->data().toInt();
            const canvas::core::TransitionType type =
                is_in ? canvas::core::TransitionType::FadeIn
                      : canvas::core::TransitionType::FadeOut;
            if (debug_enabled())
                qDebug() << "timeline: edge menu Add" << dur << "frame"
                         << (is_in ? "IN(FadeIn)" : "OUT(FadeOut)") << "on clip" << cut.a->id;
            if (is_in)
                emit transition_in_requested(cut.a, type, dur);
            else
                emit transition_requested(cut.a, type, dur);
        }
        event->accept();
        return;
    }

    ClipItem* hit_clip = nullptr;
    for (auto& item : clip_items_) {
        if (item.rect && item.rect->contains(scene_pos)) {
            hit_clip = &item;
            break;
        }
    }
    if (!hit_clip && sequence_ && scene_pos.x() >= kSceneMargin + kTrackHeaderWidth) {
        const int v_count = static_cast<int>(sequence_->video_tracks.size());
        const int total = v_count + static_cast<int>(sequence_->audio_tracks.size());
        const int clicked_track = track_at_y(scene_pos.y(), v_count);
        const int64_t frame = frame_at_x(event->pos().x());
        if (clicked_track >= 0 && clicked_track < total) {
            ClipItem* best = nullptr;
            for (auto& item : clip_items_) {
                if (item.track_index != clicked_track) continue;
                if (!item.clip) continue;
                if (frame >= item.clip->tl_in && frame < item.clip->tl_out) {
                    if (!best ||
                        item.rect->sceneBoundingRect().top() < best->rect->sceneBoundingRect().top())
                        best = &item;
                }
            }
            if (best) hit_clip = best;
        }
    }

    QAction* link_action = nullptr;
    QAction* clear_transition_action = nullptr;
    QAction* clear_transition_in_action = nullptr;
    std::vector<QAction*> transition_actions;
    std::vector<QAction*> transition_in_actions;
    if (hit_clip) {
        emit clip_selected(hit_clip->clip);
        // Checkable "Link Clips" action: checked when this clip is part of a
        // linked A/V pair; clicking it toggles the link on/off.
        link_action = menu.addAction(tr("Link Clips"));
        link_action->setCheckable(true);
        link_action->setChecked(hit_clip->clip->is_linked());

        // "Out Transition >" submenu (the clip's trailing edge). Each entry maps
        // 1:1 to a TransitionType, carrying its default duration. It is always
        // available: at a cut it crossfades into the next clip, but on a clip with
        // no right neighbour it fades that clip OUT to black at its tail.
        auto* transition_menu = menu.addMenu(tr("Out Transition") + QStringLiteral(" >"));
        transition_menu->setIcon(icon("transition_out"));
        // "In Transition >" (the clip's leading edge) fades the clip IN from black
        // at its head, with no preceding clip/cut required. Independent of the OUT
        // transition.
        auto* transition_in_menu = menu.addMenu(tr("In Transition") + QStringLiteral(" >"));
        transition_in_menu->setIcon(icon("transition_in"));
        struct Entry { const char* label; canvas::core::TransitionType type; int64_t dur; };
        static const Entry kVideo[] = {
            {"Cross Dissolve", canvas::core::TransitionType::CrossDissolve, 6},
            {"Dip to Black", canvas::core::TransitionType::DipToBlack, 6},
            {"Fade Out", canvas::core::TransitionType::FadeOut, 6},
            {"Fade In", canvas::core::TransitionType::FadeIn, 6},
            {"Wipe Left", canvas::core::TransitionType::WipeLeft, 6},
            {"Wipe Right", canvas::core::TransitionType::WipeRight, 6},
            {"Wipe Up", canvas::core::TransitionType::WipeUp, 6},
            {"Wipe Down", canvas::core::TransitionType::WipeDown, 6},
        };
        static const Entry kAudio[] = {
            {"Constant Gain", canvas::core::TransitionType::AudioFadeConstantGain, 6},
            {"Constant Power", canvas::core::TransitionType::AudioFadeConstantPower, 6},
            {"Exponential", canvas::core::TransitionType::AudioFadeExponential, 6},
        };
        const bool is_audio_track = hit_clip->track_kind == canvas::core::Track::Kind::Audio;
        const Entry* entries = is_audio_track ? kAudio : kVideo;
        const std::size_t n_entries = is_audio_track ? (sizeof(kAudio) / sizeof(kAudio[0]))
                                                     : (sizeof(kVideo) / sizeof(kVideo[0]));
        const auto add_entries = [&](QMenu* m, std::vector<QAction*>* into) {
            for (std::size_t i = 0; i < n_entries; ++i) {
                QAction* act = m->addAction(tr(entries[i].label));
                // Pack transition type + default duration into the action data.
                const qulonglong packed = (static_cast<qulonglong>(entries[i].type) << 48) |
                                          (static_cast<qulonglong>(entries[i].dur) & 0xFFFFFFFFu);
                act->setData(QVariant::fromValue(packed));
                if (into) into->push_back(act);
            }
        };
        add_entries(transition_menu, &transition_actions);
        add_entries(transition_in_menu, &transition_in_actions);
        clear_transition_action = menu.addAction(tr("Clear Out Transition"));
        clear_transition_in_action = menu.addAction(tr("Clear In Transition"));

        menu.addSeparator();
    }

    const int v_count = sequence_ ? static_cast<int>(sequence_->video_tracks.size()) : 1;
    const int a_count = sequence_ ? static_cast<int>(sequence_->audio_tracks.size()) : 0;

    // If the click is in the track-header strip, identify which channel row it
    // is over so we can offer to delete that channel.
    int header_track = -1;
    if (scene_pos.x() >= kSceneMargin && scene_pos.x() < kSceneMargin + kTrackHeaderWidth) {
        header_track = track_at_y(scene_pos.y(), v_count);
    }

    QAction* add_video = menu.addAction(tr("Add Video Channel"));
    QAction* add_audio = menu.addAction(tr("Add Audio Channel"));

    QAction* del_video = nullptr;
    QAction* del_audio = nullptr;
    if (header_track >= 0) {
        if (header_track < v_count)
            del_video = menu.addAction(tr("Delete Video Channel"));
        else
            del_audio = menu.addAction(tr("Delete Audio Channel"));
    }

    QAction* chosen = menu.exec(event->globalPos());
    if (chosen == add_video) {
        emit add_track_requested(canvas::core::Track::Kind::Video);
    } else if (chosen == add_audio) {
        emit add_track_requested(canvas::core::Track::Kind::Audio);
    } else if (chosen == del_video) {
        emit delete_track_requested(canvas::core::Track::Kind::Video, header_track);
    } else if (chosen == del_audio) {
        emit delete_track_requested(canvas::core::Track::Kind::Audio, header_track - v_count);
    } else if (chosen == link_action) {
        const int per_kind = kind_track_index(hit_clip->track_index, v_count);
        if (hit_clip->clip->is_linked())
            emit unlink_requested(hit_clip->track_kind, per_kind, hit_clip->clip->id);
        else
            emit link_requested(hit_clip->track_kind, per_kind, hit_clip->clip->id);
    } else if (chosen == clear_transition_action) {
        emit clear_transition_requested(hit_clip->clip);
    } else if (chosen == clear_transition_in_action) {
        emit clear_transition_in_requested(hit_clip->clip);
    } else if (chosen) {
        // A transition submenu entry was chosen; resolve OUT vs IN by which
        // action list the chosen action belongs to.
        auto unpack = [](QVariant v) {
            const qulonglong packed = v.toULongLong();
            const auto type = static_cast<canvas::core::TransitionType>((packed >> 48) & 0xFFFFu);
            const int64_t dur = static_cast<int64_t>(packed & 0xFFFFFFFFu);
            return std::tuple{type, dur};
        };
        auto it = std::find(transition_actions.begin(), transition_actions.end(), chosen);
        if (it != transition_actions.end()) {
            auto [type, dur] = unpack(chosen->data());
            emit transition_requested(hit_clip->clip, type, dur);
        } else {
            it = std::find(transition_in_actions.begin(), transition_in_actions.end(), chosen);
            if (it != transition_in_actions.end()) {
                auto [type, dur] = unpack(chosen->data());
                emit transition_in_requested(hit_clip->clip, type, dur);
            }
        }
    }
    event->accept();
}

}  // namespace canvas::gui
