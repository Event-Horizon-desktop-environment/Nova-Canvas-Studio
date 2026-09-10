#include "Widgets/timeline_widget.hpp"
#include "Logging.hpp"
#include "UX/theme.hpp"

#include <QColor>
#include <QContextMenuEvent>
#include <QMenu>
#include <QPainter>
#include <QPaintEvent>
#include <QScrollBar>
#include <QKeyEvent>
#include <QWheelEvent>
#include <QGraphicsItem>
#include <QGraphicsRectItem>
#include <QMetaEnum>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <typeinfo>
#include <utility>

namespace canvas::gui {

TimelineWidget::~TimelineWidget() {
    // Detach the scene while it (and every item it owns, including the pinned
    // chrome groups) is still intact. QGraphicsScene::~QGraphicsScene re-arms
    // each attached view via QGraphicsView::setScene(nullptr), which recalculates
    // the scrollbar range and calls scrollContentsBy() — touching torn-down
    // items there (top_pinned_ et al.) is a use-after-free. Detaching first
    // keeps member (scene_) -> base (QGraphicsView) teardown clean.
    if (scene() == &scene_) setScene(nullptr);
}

TimelineWidget::TimelineWidget(QWidget* parent) : QGraphicsView(parent) {
    setScene(&scene_);
    // Default raster backing store. A QOpenGLWidget viewport used to render the
    // scene here, but QGraphicsView's GL paint engine re-uploads every visible
    // QPixmap (filmstrip cells, waveform strips) to a texture on EACH repaint,
    // which cost ~2.3ms per pixmap (40-70ms/frame with thumbnails visible).
    // Raster paint is backed by the widget backing store, so pixmap draws are
    // cached blits; the scene is flat rects/paths/pixmaps, so nothing needs GL.
    setRenderHint(QPainter::Antialiasing, true);
    setDragMode(QGraphicsView::NoDrag);
    setMouseTracking(true);
    setTransformationAnchor(QGraphicsView::AnchorUnderMouse);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOn);
    setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOn);
    setAlignment(Qt::AlignLeft | Qt::AlignTop);
    setAcceptDrops(true);
    scene_.setBackgroundBrush(tokens().surface);
    // Manual scrollbar interaction must not be defeated by playhead-follow:
    // QGraphicsView delivers wheel/press events to the scrollbar (not the view),
    // so watch both directly and treat any such gesture as "user navigated away".
    if (horizontalScrollBar()) horizontalScrollBar()->installEventFilter(this);
    if (verticalScrollBar()) verticalScrollBar()->installEventFilter(this);
    // The whole scene re-derives its colors from the active tokens, so rebuild
    // it whenever the appearance mode flips (see set_light()).
    register_theme_reapply([this] { rebuild_timeline(); });
}

bool TimelineWidget::eventFilter(QObject* watched, QEvent* event) {
    // Manual scrollbar interaction (press, thumb-drag, wheel) is navigation:
    // stop following the playhead so the next rebuild/playhead update doesn't
    // yank the view back. All events pass through untouched.
    if (watched == horizontalScrollBar() || watched == verticalScrollBar()) {
        const auto type = event->type();
        if (type == QEvent::Wheel || type == QEvent::MouseButtonPress ||
            (type == QEvent::MouseMove &&
             static_cast<QMouseEvent*>(event)->buttons() != Qt::NoButton)) {
            follow_playhead_ = false;
        }
    }
    return QGraphicsView::eventFilter(watched, event);
}

void TimelineWidget::set_sequence(const canvas::core::Sequence* sequence) {
    sequence_ = sequence;
    // Reconcile the ruler/playhead timecode rate from the live sequence so a
    // project (re)loaded at a different fps re-derives its tick labels instead
    // of carrying the previous project's rate.
    if (sequence && sequence->fps > 0.0) fps_ = sequence->fps;
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
    // Clamp the low end to the 100% baseline so fitting a tiny clip lands on
    // one frame per pixel (whole clip visible) instead of pinning the zoom-in
    // max; the high end guards absurdly long timelines.
    set_frames_per_pixel(std::clamp(static_cast<double>(dur) / view_w,
                                    kDefaultFramesPerPixel, kMaxFramesPerPixel));
}

double TimelineWidget::interactive_floor_percent() const {
    if (!sequence_) return kZoomMinPercent;
    const int view_w = std::max(viewport()->width() - kTrackHeaderWidth - 40, 1);
    const int64_t dur = std::max<int64_t>(sequence_->duration_frames(), 1);
    // Fit math scaled 5x further out than zoom_fit's "whole timeline fills the
    // viewport" target, so the zoom-out end still has room to roam past fit;
    // never sharper than the 100% baseline (a clip that needs that is already
    // fully visible) and never beyond the hard frames-per-pixel cap.
    const double fit_fpp = std::clamp(5.0 * static_cast<double>(dur) / view_w,
                                      kDefaultFramesPerPixel, kMaxFramesPerPixel);
    return kDefaultFramesPerPixel / fit_fpp * 100.0;
}

void TimelineWidget::set_zoom_percent(double percent) {
    const double floor = std::min(interactive_floor_percent(), kZoomMaxPercent);
    set_frames_per_pixel(kDefaultFramesPerPixel /
                         std::clamp(percent, floor, kZoomMaxPercent) * 100.0);
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

int TimelineWidget::desired_timeline_height() const {
    // Dock height that shows every channel flush at the parked scroll position.
    // At park (scroll = pan room + top pad) the viewport reveals scene rows from
    // `park` downward, so the viewport must be (content bottom - park) tall; the
    // +40 covers the dock title bar + frame margins. 0 while the timeline holds
    // no clips yet (the caller keeps the compact floor). Only computed off
    // geometry that changes with the channel count, so zoom/scrub never resize
    // the dock.
    if (!sequence_ || !has_timeline_content()) return 0;
    const int v_count = static_cast<int>(sequence_->video_tracks.size());
    const int a_count = static_cast<int>(sequence_->audio_tracks.size());
    const double content_bottom = static_cast<double>(kSceneMargin) + tracks_content_height(v_count, a_count);
    const double park = pan_down_room_ + track_v_pad_top_;
    const double needed_vp = std::max(0.0, content_bottom - park);
    return std::max(265, static_cast<int>(std::ceil(needed_vp + 40.0)));
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
    if (static_cast<int>(video_track_heights_.size()) != vn) {
        video_track_heights_.assign(static_cast<std::size_t>(vn), kDefaultTrackHeight);
        track_count_changed_ = true;
    }
    if (static_cast<int>(audio_track_heights_.size()) != an) {
        audio_track_heights_.assign(static_cast<std::size_t>(an), kDefaultTrackHeight);
        track_count_changed_ = true;
    }
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
    // Any wheel invocation (scroll or Ctrl+zoom) is manual navigation: the user
    // wants to look somewhere else, so stop following the playhead until the
    // next explicit playhead jump re-enables it.
    follow_playhead_ = false;
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
    const auto rz_t0 = std::chrono::steady_clock::now();
    QGraphicsView::resizeEvent(event);
    // A pure viewport resize must NOT run the full scene rebuild (that tore
    // down every clip item + re-requested thumbnails on each resize tick — the
    // "changing height is slow on clip-heavy timelines" stall). relayout_scene()
    // only re-renders the view chrome; clip items survive. If the chrome was
    // never built yet (pre-first-layout), fall back to the full build.
    const bool full_rebuild = !(chrome_ && top_pinned_);
    if (chrome_ && top_pinned_)
        relayout_scene();
    else
        rebuild_timeline();
    update_playhead_position(playhead_frame_);
    update_minimap_viewport();
    // Per-resize cost of the whole UX path on the UI thread. Sustained
    // ms_avg >> frame budget while dragging a dock edge = "resizing is sticky".
    // The relayout-vs-rebuild split tells you whether a viewer/dock resize is
    // still paying full scene teardown (then relayout_scene isn't routing here)
    // or is dominated by the chrome-only path.
    const double rz_ms = std::chrono::duration<double, std::milli>(
                             std::chrono::steady_clock::now() - rz_t0).count();
    static auto s_rz_at = std::chrono::steady_clock::now();
    static int s_rz_n = 0;
    static double s_rz_ms = 0.0, s_max_ms = 0.0;
    ++s_rz_n;
    s_rz_ms += rz_ms;
    s_max_ms = std::max(s_max_ms, rz_ms);
    const auto rz_now = std::chrono::steady_clock::now();
    if (s_rz_n == 1 || rz_now - s_rz_at >= std::chrono::seconds(1)) {
        s_rz_at = rz_now;
        qDebug() << "[ui:timeline] resize ms_avg=" << QString::number(s_rz_ms / s_rz_n, 'f', 2)
                   << "ms_last=" << QString::number(rz_ms, 'f', 2)
                   << "ms_max=" << QString::number(s_max_ms, 'f', 2)
                   << "resizes/s=" << s_rz_n
                   << "full_rebuild=" << (full_rebuild ? 1 : 0)
                   << "clips=" << (sequence_ ? static_cast<int>(clip_items_.size()) : 0);
        s_rz_n = 0;
        s_rz_ms = 0.0;
        s_max_ms = 0.0;
    }
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

void TimelineWidget::paintEvent(QPaintEvent* event) {
    const auto pt_t0 = std::chrono::steady_clock::now();
    QGraphicsView::paintEvent(event);
    // Timeline scene paint cost on the UI thread. This is the OTHER half of a
    // resize cycle: relayout_scene() rebuilds chrome in ~0.2ms, then the scene
    // has to actually repaint the whole viewport here. Sustained paint time
    // >> the relayout number on a clip-heavy timeline = the scene items
    // themselves (not the chrome) are the paint bottleneck.
    const double pt_ms = std::chrono::duration<double, std::milli>(
                             std::chrono::steady_clock::now() - pt_t0).count();
    static auto s_pt_at = std::chrono::steady_clock::now();
    static int s_pt_n = 0;
    static double s_pt_ms = 0.0, s_max_ms = 0.0;
    ++s_pt_n;
    s_pt_ms += pt_ms;
    s_max_ms = std::max(s_max_ms, pt_ms);
    const auto pt_now = std::chrono::steady_clock::now();
    if (s_pt_n == 1 || pt_now - s_pt_at >= std::chrono::seconds(1)) {
        // Per-class count of the items intersecting the visible viewport, so a
        // paint hotspot can be attributed to a specific item type (pixmap
        // uploads vs vector paths vs text) rather than just "the scene".
        const QRectF visible = mapToScene(viewport()->rect()).boundingRect();
        const QRectF exposed = visible.intersected(scene_.sceneRect());
        QHash<QString, int> classes;
        int n_visible = 0;
        const QList<QGraphicsItem*> hit = scene_.items(exposed, Qt::IntersectsItemShape);
        for (QGraphicsItem* it : hit) {
            ++n_visible;
            ++classes[QLatin1String(typeid(*it).name())];
        }
        QStringList cls;
        QList<QString> keys = classes.keys();
        std::sort(keys.begin(), keys.end(),
                  [&](const QString& a, const QString& b) { return classes[a] > classes[b]; });
        for (const QString& k : keys) cls << (k + "=" + QString::number(classes[k]));
        s_pt_at = pt_now;
        qDebug() << "[ui:timeline] paint ms_avg=" << QString::number(s_pt_ms / s_pt_n, 'f', 2)
                   << " ms_last=" << QString::number(pt_ms, 'f', 2)
                   << " ms_max=" << QString::number(s_max_ms, 'f', 2)
                   << " paints/s=" << s_pt_n
                   << " total=" << static_cast<int>(scene_.items().size())
                   << " visible=" << n_visible
                   << " classes=[" << cls.join(", ") << "]";
        s_pt_n = 0;
        s_pt_ms = 0.0;
        s_max_ms = 0.0;
    }
}

void TimelineWidget::contextMenuEvent(QContextMenuEvent* event) {
    if (!sequence_) {
        QGraphicsView::contextMenuEvent(event);
        return;
    }
    const QPointF scene_pos = mapToScene(event->pos());
    QMenu menu(this);
    apply_rounded_menu(&menu);

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
        apply_rounded_menu(transition_menu);
        // "In Transition >" (the clip's leading edge) fades the clip IN from black
        // at its head, with no preceding clip/cut required. Independent of the OUT
        // transition.
        auto* transition_in_menu = menu.addMenu(tr("In Transition") + QStringLiteral(" >"));
        transition_in_menu->setIcon(icon("transition_in"));
        apply_rounded_menu(transition_in_menu);
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
