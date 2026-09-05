#include "Widgets/timeline_widget.hpp"
#include "Widgets/timeline_snap.hpp"
#include "Logging.hpp"

#include <QApplication>
#include <QColor>
#include <QDateTime>
#include <QMouseEvent>
#include <QPen>
#include <QBrush>
#include <QPointF>
#include <QRectF>
#include <QGraphicsItem>
#include <QGraphicsRectItem>
#include <QGraphicsTextItem>
#include <QGraphicsPixmapItem>
#include <QGraphicsPathItem>
#include <QGraphicsBlurEffect>
#include <QScrollBar>
#include <QPainterPath>
#include <QPainter>
#include <QSvgRenderer>
#include <QPixmap>
#include <QFont>
#include <QLineF>
#include <QDragEnterEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QMimeData>
#include <QUrl>

#include <algorithm>
#include <cmath>

namespace canvas::gui {

int TimelineWidget::track_at_y(double scene_y, int v_count) const {
    // No clip on any track yet: the rows are hidden, so no row is hit-testable.
    if (!has_timeline_content()) return -1;
    const int total = v_count + (sequence_ ? static_cast<int>(sequence_->audio_tracks.size()) : 0);
    for (int i = 0; i < total; ++i) {
        const double t = track_top(i, v_count);
        if (scene_y >= t && scene_y < t + track_height(i, v_count)) return i;
    }
    return -1;
}

int TimelineWidget::kind_track_index(int flat_track, int v_count) const {
    return flat_track < v_count ? flat_track : flat_track - v_count;
}

int64_t TimelineWidget::snap_frame(int64_t frame) const {
    if (!snap_enabled_ || fps_ <= 0.0) return frame;
    return timeline_snap::snap_to_grid(frame, frames_per_pixel_);
}

TimelineWidget::ClipItem* TimelineWidget::find_linked_mate(ClipItem* item) {
    if (!item || !item->clip || !item->clip->is_linked()) return nullptr;
    const canvas::core::ClipId lk = item->clip->linked_id;
    for (auto& other : clip_items_) {
        if (&other == item) continue;
        if (other.clip && other.clip->id == lk &&
            other.track_kind != item->track_kind) {
            return &other;
        }
    }
    return nullptr;
}

void TimelineWidget::position_clip_at(ClipItem& item, int64_t tl_in) {
    if (!item.rect) return;
    const int v_count = sequence_ ? static_cast<int>(sequence_->video_tracks.size()) : 1;
    const double left = kSceneMargin + kTrackHeaderWidth + tl_in / frames_per_pixel_;
    const double top = static_cast<double>(track_top(item.track_index, v_count));
    const double cy = top + (item.track_kind == canvas::core::Track::Kind::Video ? 2 : 3);

    // Move the clip's paint-clip group (and with it every child: filmstrip, shell,
    // outline, label) as one unit. Translating only the children would let the
    // thumbnails wander out of the clip's rect (ItemClipsChildrenToShape clips
    // to a fixed local box), so apply the delta to the group itself.
    QGraphicsItem* group = item.rect->parentItem();
    if (!group) return;
    const QPointF new_base(left, cy);
    const QPointF delta = new_base - item.rect->scenePos();
    group->moveBy(delta.x(), delta.y());
}

bool TimelineWidget::reacquire_dragged_clip(const canvas::core::ClipId id,
                                            const canvas::core::ClipId mate_id,
                                            const int64_t pointer_frame) {
    if (!is_dragging_ || !dragged_clip_) return false;
    for (auto& it : clip_items_) {
        if (!it.clip || it.clip->id != id) continue;
        dragged_clip_ = &it;
        original_track_index_ = it.track_index;
        drag_ctrl_.begin(pointer_frame, it.clip->tl_in, it.track_index);
        drag_mate_ = find_linked_mate(&it);
        // Fallback: if the move severed the link (fresh track, mate not linked back
        // yet), re-attach the press-time mate by id so it keeps riding along.
        if (!drag_mate_ && mate_id != 0) {
            for (auto& other : clip_items_) {
                if (&other == &it || !other.clip || other.clip->id != mate_id) continue;
                drag_mate_ = &other;
                break;
            }
        }
        return true;
    }
    return false;
}

void TimelineWidget::scrub_to_frame(const int64_t frame) {
    static int scrub_log_ = 0;
    if (debug_enabled() && (scrub_log_++ % 30) == 0)
        qDebug() << "timeline: scrub_to_frame" << frame;
    if ((scrub_log_++ & 3u) == 0u) {
        static int64_t last_log = -1;
        static auto last_t = QDateTime::currentDateTime();
        static bool first = true;
        auto now = QDateTime::currentDateTime();
        double dt_ms = 0.0;
        if (!first) dt_ms = last_t.msecsTo(now);
        qWarning() << "[scrub] TIMELINE scrub_to_frame=" << frame
                   << "dt_ms=" << dt_ms
                   << "delta=" << (last_log >= 0 ? (frame - last_log) : 0);
        last_log = frame;
        last_t = now;
        first = false;
    }
    set_playhead_position(frame);
    emit playhead_moved(frame);
}

void TimelineWidget::update_blade_preview(const int64_t frame) {
    const double top = kMinimapHeight + kSceneMargin;
    const double x = kSceneMargin + kTrackHeaderWidth + frame / frames_per_pixel_;
    if (!blade_preview_item_) {
        blade_preview_item_ = scene_.addLine(
            QLineF(x, top, x, scene_.sceneRect().bottom()),
            QPen(QColor(0xFF, 0xAA, 0x00), 1, Qt::DashLine));
        blade_preview_item_->setZValue(95);
    }
    blade_preview_item_->setLine(QLineF(x, top, x, scene_.sceneRect().bottom()));
    blade_preview_item_->setVisible(true);
}

void TimelineWidget::hide_blade_preview() {
    if (blade_preview_item_) blade_preview_item_->setVisible(false);
}

void TimelineWidget::update_snap_indicator(const bool snapped, const int64_t frame) {
    const double top = kMinimapHeight + kSceneMargin;
    const double x = kSceneMargin + kTrackHeaderWidth + frame / frames_per_pixel_;
    if (snapped) {
        if (!snap_indicator_item_) {
            snap_indicator_item_ = scene_.addLine(
                QLineF(x, top, x, scene_.sceneRect().bottom()),
                QPen(QColor(0x4C, 0x92, 0xFF), 1, Qt::DashLine));
            snap_indicator_item_->setZValue(94);
        }
        snap_indicator_item_->setLine(QLineF(x, top, x, scene_.sceneRect().bottom()));
        snap_indicator_item_->setVisible(true);
    } else if (snap_indicator_item_) {
        snap_indicator_item_->setVisible(false);
    }
}

void TimelineWidget::hide_snap_indicator() {
    if (snap_indicator_item_) snap_indicator_item_->setVisible(false);
}

namespace {
QPainterPath canvas_rounded_rect_path(const QRectF& r, qreal radius) {
    QPainterPath p;
    p.addRoundedRect(r, std::min<qreal>(radius, r.height() / 2.0),
                     std::min<qreal>(radius, r.width() / 2.0));
    return p;
}
}  // namespace

// --- Transition cut-handle editor ---------------------------------------------

TimelineWidget::CutTarget TimelineWidget::cut_at_scene_pos(const QPointF& p) const {
    CutTarget t;
    if (!sequence_ || p.x() < kSceneMargin + kTrackHeaderWidth) return t;
    const int v_count = static_cast<int>(sequence_->video_tracks.size());
    const int flat = track_at_y(p.y(), v_count);
    if (flat < 0) return t;
    const auto& track = flat < v_count
        ? sequence_->video_tracks[static_cast<std::size_t>(flat)]
        : sequence_->audio_tracks[static_cast<std::size_t>(flat - v_count)];
    const canvas::core::Track::Kind kind =
        flat < v_count ? canvas::core::Track::Kind::Video : canvas::core::Track::Kind::Audio;

    const double px = p.x();
    double best_dist = 1e18;
    // First: genuine cuts (two adjacent clips sharing A.tl_out == B.tl_in).
    for (const auto& a : track.clips) {
        for (const auto& b : track.clips) {
            if (&a == &b) continue;
            if (b.tl_in != a.tl_out) continue;  // adjacent on the same track = a cut
            const double cut_x =
                kSceneMargin + kTrackHeaderWidth + a.tl_out / frames_per_pixel_;
            const double d = std::abs(px - cut_x);
            if (d <= kCutHoverTolerancePx && d < best_dist) {
                best_dist = d;
                t.a = &a;
                t.b = &b;
                t.kind = kind;
                t.track_index = flat;
                t.cut_frame = a.tl_out;
                t.edge = Edge::Cut;
            }
        }
    }

    // With no cut under the pointer, offer a single-clip edge handle: the clip's
    // leading edge (IN fade) at tl_in, or its trailing edge (OUT fade) at tl_out.
    // Skip an edge already shared with a cut (handled above).
    if (!t.valid()) {
        for (const auto& c : track.clips) {
            auto consider = [&](int64_t frame, Edge edge, const canvas::core::Clip* prime) {
                const double x = kSceneMargin + kTrackHeaderWidth + frame / frames_per_pixel_;
                const double d = std::abs(px - x);
                if (d <= kCutHoverTolerancePx && d < best_dist) {
                    best_dist = d;
                    t.a = prime;
                    t.b = nullptr;
                    t.kind = kind;
                    t.track_index = flat;
                    t.cut_frame = frame;
                    t.edge = edge;
                }
            };
            // Leading edge (fade in) — always offered; it has no left neighbour.
            consider(c.tl_in, Edge::Start, &c);
            // Trailing edge (fade out) — only when nothing starts exactly here
            // (that case is a cut handled above).
            bool has_right_neighbour = false;
            for (const auto& o : track.clips)
                if (o.tl_in == c.tl_out) { has_right_neighbour = true; break; }
            if (!has_right_neighbour) consider(c.tl_out, Edge::End, &c);
        }
    }
    return t;
}

// Largest legal transition duration: on a cut, bounded by the shorter of the
// two neighbouring clips; on a single-clip edge, by that clip's own duration.
int64_t TimelineWidget::transition_max_duration(const CutTarget& t) {
    if (!t.valid()) return kMinTransitionFrames;
    if (t.is_cut()) {
        const int64_t maxd = std::min<int64_t>(t.a->duration(), t.b->duration());
        return std::max<int64_t>(kMinTransitionFrames, maxd);
    }
    return std::max<int64_t>(kMinTransitionFrames, t.a->duration());
}

void TimelineWidget::update_transition_hover(const QPointF& p) {
    // While actively dragging a resize edge, keep the current editor alive and
    // update it from move_transition_handle instead.
    if (transition_handle_dragging_) return;

    const CutTarget t = cut_at_scene_pos(p);
    if (!t.valid()) {
        hide_transition_handle();
        return;
    }

    if (!transition_handle_visible_ || transition_target_.cut_frame != t.cut_frame ||
        transition_target_.track_index != t.track_index || transition_target_.a != t.a ||
        transition_target_.b != t.b || transition_target_.edge != t.edge) {
        // (Re)open the editor for this target.
        transition_target_ = t;
        // For a single-clip Start (IN) edge, seed from the clip's IN duration;
        // otherwise (Cut / End) from its OUT duration.
        int64_t init = t.edge == Edge::Start ? t.a->transition_in_duration
                                                        : t.a->transition_out_duration;
        if (init < kMinTransitionFrames) init = 6;
        init = std::clamp(init, kMinTransitionFrames, transition_max_duration(t));
        transition_handle_duration_ = init;
        transition_handle_left_frame_ =
            t.edge == Edge::Start ? t.cut_frame
            : t.edge == Edge::End ? t.cut_frame - init
                                  : t.cut_frame - init / 2;
        transition_handle_right_frame_ =
            t.edge == Edge::Start ? t.cut_frame + init
            : t.edge == Edge::End ? t.cut_frame
                                  : t.cut_frame + (init - init / 2);
        transition_handle_visible_ = true;
        rebuild_transition_handle();
    }

    if (transition_handle_visible_ &&
        p.x() <= kSceneMargin + kTrackHeaderWidth) {
        hide_transition_handle();
    }
}

void TimelineWidget::hide_transition_handle() {
    if (transition_handle_visible_) unsetCursor();
    transition_handle_visible_ = false;
    transition_handle_dragging_ = false;
    transition_drag_edge_ = kTransitionEdgeNone;
    transition_snap_presets_ = false;
    transition_press_armed_ = false;
    transition_target_ = CutTarget{};
    for (QGraphicsItem* item : transition_items_) {
        if (item->scene() == &scene_) scene_.removeItem(item);
        delete item;
    }
    transition_items_.clear();
    transition_overlay_ = nullptr;
    transition_icon_ = nullptr;
}

void TimelineWidget::rebuild_transition_handle() {
    if (!transition_target_.valid() || !transition_handle_visible_) return;

    // Tear down the previous handle items.
    for (QGraphicsItem* item : transition_items_) {
        if (item->scene() == &scene_) scene_.removeItem(item);
        delete item;
    }
    transition_items_.clear();
    transition_overlay_ = nullptr;
    transition_icon_ = nullptr;

    const int v_count = static_cast<int>(sequence_ ? sequence_->video_tracks.size() : 1);
    const double track_top_y =
        static_cast<double>(track_top(transition_target_.track_index, v_count));
    const double row_y = track_top_y + 3;
    const double row_h = track_height(transition_target_.track_index, v_count) - 8.0;

    auto scene_x = [this](int64_t frame) {
        return kSceneMargin + kTrackHeaderWidth + frame / frames_per_pixel_;
    };
    const double cut_x = scene_x(transition_target_.cut_frame);
    // For a single-clip edge the overlay extends into the clip from the boundary
    // (Start: rightward from tl_in; End: leftward from tl_out); a cut centers it.
    const bool is_edge = !transition_target_.is_cut();
    double left_x, right_x;
    if (is_edge && transition_target_.edge == Edge::Start) {
        left_x = cut_x;
        right_x = cut_x + transition_handle_duration_ / frames_per_pixel_;
    } else if (is_edge && transition_target_.edge == Edge::End) {
        left_x = cut_x - transition_handle_duration_ / frames_per_pixel_;
        right_x = cut_x;
    } else {
        left_x = scene_x(transition_handle_left_frame_);
        right_x = scene_x(transition_handle_right_frame_);
    }
    const double overlay_w = std::max(2.0, right_x - left_x);

    // Semi-rounded, semi-transparent overlay marking the transition span. No
    // outline — the SVG glyph is the visual focus. A tiny blur frosts the tint
    // so the preview matches the persistent glass bubbles.
    const QRectF ovr(left_x, row_y, overlay_w, row_h);
    transition_overlay_ =
        scene_.addPath(canvas_rounded_rect_path(ovr, 7.0), QPen(Qt::NoPen),
                       QBrush(QColor(64, 160, 255, 60)));
    auto* frost = new QGraphicsBlurEffect;
    frost->setBlurRadius(1.2);
    transition_overlay_->setGraphicsEffect(frost);
    transition_overlay_->setZValue(60);
    transition_items_.push_back(transition_overlay_);

    // Directional indicator at the boundary, drawn from the bundled SVG glyphs:
    // a cut uses `<[]>` (centred arrows), a Start edge `<[` (fade-in), an End
    // edge `]>` (fade-out). The icon always sits at the boundary (edit point).
    const double icx = cut_x;
    const double icy = row_y + row_h / 2.0;
    const QString icon_name =
        transition_target_.edge == Edge::Start   ? QStringLiteral("transition_in")
        : transition_target_.edge == Edge::End   ? QStringLiteral("transition_out")
                                                 : QStringLiteral("transition");
    constexpr int kIconPx = 48;
    QPixmap glyph(kIconPx, kIconPx);
    glyph.fill(Qt::transparent);
    {
        QSvgRenderer renderer(QStringLiteral(":/icons/%1.svg").arg(icon_name));
        QPainter p(&glyph);
        renderer.render(&p, QRectF(0, 0, kIconPx, kIconPx));
        p.end();
    }
    QPixmap tinted(kIconPx, kIconPx);
    tinted.fill(QColor(255, 255, 255, 235));
    {
        QPainter tp(&tinted);
        tp.setCompositionMode(QPainter::CompositionMode_SourceIn);
        tp.drawPixmap(0, 0, glyph);
        tp.end();
    }
    auto* icon = scene_.addPixmap(tinted);
    icon->setPos(QPointF(icx - kIconPx / 2.0, icy - kIconPx / 2.0));
    icon->setTransformationMode(Qt::SmoothTransformation);
    icon->setZValue(61);
    transition_icon_ = icon;
    transition_items_.push_back(icon);

    // Duration label intentionally omitted: the hover overlay shows only the
    // shape + icon (no frame-count text on the canvas — read it in the context
    // menu instead).
    transition_items_.push_back(transition_overlay_);
    transition_items_.push_back(icon);
}

void TimelineWidget::press_transition_handle(const QPointF& p) {
    if (!transition_handle_visible_ || !transition_target_.valid()) return;
    // The hover preview must never create a transition: only targets that ALREADY
    // carry one can be grabbed for a resize (transitions are added exclusively
    // via the right-click context menu).
    const bool has_transition =
        transition_target_.is_cut()
            ? (transition_target_.a->has_transition_out() ||
               transition_target_.b->has_transition_in())
            : (transition_target_.edge == Edge::Start
                   ? transition_target_.a->has_transition_in()
                   : transition_target_.a->has_transition_out());
    if (!has_transition) return;
    const int64_t left = transition_handle_left_frame_;
    const int64_t right = transition_handle_right_frame_;
    const double lx = kSceneMargin + kTrackHeaderWidth + left / frames_per_pixel_;
    const double rx = kSceneMargin + kTrackHeaderWidth + right / frames_per_pixel_;
    constexpr double kEdgeGrab = 7.0;

    int edge = kTransitionEdgeNone;
    if (std::abs(p.x() - lx) <= kEdgeGrab) edge = kTransitionEdgeLeft;
    else if (std::abs(p.x() - rx) <= kEdgeGrab) edge = kTransitionEdgeRight;
    if (edge < 0) return;

    transition_handle_dragging_ = true;
    transition_drag_edge_ = edge;
    // The opposite edge stays fixed while the grabbed edge moves.
    transition_drag_anchor_frame_ =
        edge == kTransitionEdgeLeft ? right : left;
    setCursor(Qt::SizeHorCursor);
}

void TimelineWidget::move_transition_handle(const QPointF& p) {
    if (!transition_handle_dragging_) return;
    const int64_t edge_frame = static_cast<int64_t>(
        (p.x() - kSceneMargin - kTrackHeaderWidth) * frames_per_pixel_);
    const int64_t maxd = transition_max_duration(transition_target_);
    const int64_t rawsize = edge_frame > transition_drag_anchor_frame_
                                ? edge_frame - transition_drag_anchor_frame_
                                : transition_drag_anchor_frame_ - edge_frame;
    int64_t dur = std::clamp(rawsize, kMinTransitionFrames, maxd);
    if (transition_snap_presets_) {
        // Bubble drags snap to the favourite presets so the live label settles on a
        // clean duration instead of a free-form frame count.
        static constexpr int64_t kFavoritePresets[] = {14, 30, 60, 120};
        int64_t best = dur;
        for (const int64_t preset : kFavoritePresets) {
            if (preset < kMinTransitionFrames || preset > maxd) continue;
            if (std::llabs(preset - dur) < std::llabs(best - dur)) best = preset;
        }
        dur = best;
    }
    transition_handle_duration_ = dur;
    if (transition_drag_edge_ == kTransitionEdgeLeft) {
        // Anchor is the (right) edge that doesn't move.
        const int64_t right = transition_drag_anchor_frame_;
        transition_handle_left_frame_ = right - dur;
        transition_handle_right_frame_ = right;
    } else {
        const int64_t left = transition_drag_anchor_frame_;
        transition_handle_left_frame_ = left;
        transition_handle_right_frame_ = left + dur;
    }
    rebuild_transition_handle();

    // Keep the persistent bubble live-synced to the dragged duration.
    if (transition_snap_presets_ && transition_target_.valid()) {
        const bool in_edge = transition_target_.edge == Edge::Start;
        for (auto& b : transition_bubbles_) {
            if (b.pill && b.in_edge == in_edge && b.clip_id == transition_target_.a->id)
                refresh_transition_bubble(b, transition_handle_duration_);
        }
    }
}

void TimelineWidget::release_transition_handle() {
    if (!transition_handle_dragging_) return;
    transition_handle_dragging_ = false;
    transition_drag_edge_ = kTransitionEdgeNone;
    transition_snap_presets_ = false;
    unsetCursor();
    // A plain bubble click (no drag) must NOT commit an edit: on a one-sided
    // cut the old code re-emitted into the complementary clip and materialised
    // a fresh transition where none existed. Only resize when the duration
    // actually differs from what is already stored at the edit point.
    if (transition_target_.valid()) {
        const int64_t stored = transition_target_.is_cut()
                                   ? std::max(transition_target_.a->transition_out_duration,
                                              transition_target_.b->transition_in_duration)
                                   : (transition_target_.edge == Edge::Start
                                          ? transition_target_.a->transition_in_duration
                                          : transition_target_.a->transition_out_duration);
        if (transition_handle_duration_ != stored) {
            if (transition_target_.edge == Edge::Start)
                emit transition_in_resized(transition_target_.a, transition_handle_duration_);
            else
                emit transition_resized(transition_target_.a, transition_handle_duration_);
        }
    }
    // With the hover editor removed, the overlay opened by a bubble drag would
    // otherwise linger on screen; tear it down now the edit commits.
    hide_transition_handle();
}

void TimelineWidget::open_transition_editor_for_clip(const canvas::core::Clip* clip,
                                                     int64_t frame, bool in_edge) {
    if (!clip || !sequence_) return;
    const int v_count = static_cast<int>(sequence_->video_tracks.size());
    int flat = -1;
    canvas::core::Track::Kind kind = canvas::core::Track::Kind::Video;
    for (int i = 0; i < v_count; ++i) {
        if (sequence_->video_tracks[i].clip_with_id(clip->id)) {
            flat = i;
            break;
        }
    }
    if (flat < 0) {
        const int a_count = static_cast<int>(sequence_->audio_tracks.size());
        for (int i = 0; i < a_count; ++i) {
            if (sequence_->audio_tracks[i].clip_with_id(clip->id)) {
                flat = v_count + i;
                kind = canvas::core::Track::Kind::Audio;
                break;
            }
        }
    }
    if (flat < 0) return;

    transition_target_ = CutTarget{clip, nullptr, kind, flat, frame,
                                   in_edge ? Edge::Start : Edge::End};
    const int64_t seeded = in_edge ? clip->transition_in_duration : clip->transition_out_duration;
    const int64_t init = std::clamp(seeded >= kMinTransitionFrames ? seeded : 6,
                                    kMinTransitionFrames, transition_max_duration(transition_target_));
    transition_handle_duration_ = init;
    transition_handle_left_frame_ = in_edge ? frame : frame - init;
    transition_handle_right_frame_ = in_edge ? frame + init : frame;
    transition_handle_visible_ = true;
    transition_snap_presets_ = false;
    rebuild_transition_handle();
}

bool TimelineWidget::maybe_press_transition_bubble(const QPointF& p) {
    for (std::size_t i = 0; i < transition_bubbles_.size(); ++i) {
        auto& b = transition_bubbles_[i];
        if (!b.hit.contains(p)) continue;
        const canvas::core::Clip* clip = nullptr;
        if (sequence_) {
            for (const auto& t : sequence_->video_tracks)
                if ((clip = t.clip_with_id(b.clip_id))) break;
            if (!clip)
                for (const auto& t : sequence_->audio_tracks)
                    if ((clip = t.clip_with_id(b.clip_id))) break;
        }
        if (!clip) continue;

        const canvas::core::Clip* bclip = nullptr;
        if (b.cut && b.b_clip_id) {
            if (sequence_) {
                for (const auto& t : sequence_->video_tracks)
                    if ((bclip = t.clip_with_id(b.b_clip_id))) break;
                if (!bclip)
                    for (const auto& t : sequence_->audio_tracks)
                        if ((bclip = t.clip_with_id(b.b_clip_id))) break;
            }
            if (!bclip) continue;
        }

        // Clicking a bubble selects the transition (Delete removes it). A
        // bubble-initiated resize drag is ARM-LAZY: it engages only after the
        // pointer travels past the drag threshold, so a plain click can never
        // open the editor, edit the duration, or create a transition.
        select_transition_bubble(i);
        transition_press_armed_ = true;
        transition_press_pos_ = p;
        transition_press_a_ = clip;
        transition_press_b_ = bclip;
        transition_press_frame_ = b.frame;
        transition_press_in_edge_ = !b.cut && b.in_edge;
        return true;
    }
    return false;
}

void TimelineWidget::engage_transition_drag(const QPointF& scene_pos) {
    if (!transition_press_armed_ || !transition_press_a_) return;
    if (transition_press_b_)
        open_transition_editor_for_cut(transition_press_a_, transition_press_b_,
                                       transition_press_frame_);
    else
        open_transition_editor_for_clip(transition_press_a_, transition_press_frame_,
                                        transition_press_in_edge_);

    // Grabbing anywhere on the bubble behaves like grabbing the overlay's
    // resize handle nearest to the pointer, snapping to the presets all drag.
    const double lx = kSceneMargin + kTrackHeaderWidth +
                      transition_handle_left_frame_ / frames_per_pixel_;
    const double rx = kSceneMargin + kTrackHeaderWidth +
                      transition_handle_right_frame_ / frames_per_pixel_;
    const double dl = std::abs(scene_pos.x() - lx);
    const double dr = std::abs(scene_pos.x() - rx);
    transition_handle_dragging_ = true;
    transition_drag_edge_ = dr < dl ? kTransitionEdgeRight : kTransitionEdgeLeft;
    transition_drag_anchor_frame_ =
        transition_drag_edge_ == kTransitionEdgeLeft ? transition_handle_right_frame_
                                                     : transition_handle_left_frame_;
    transition_snap_presets_ = true;
    setCursor(Qt::SizeHorCursor);
    transition_press_armed_ = false;
}

void TimelineWidget::select_transition_bubble(std::size_t index) {
    if (index >= transition_bubbles_.size()) return;
    const auto& b = transition_bubbles_[index];
    selected_transition_.a = b.clip_id;
    selected_transition_.b = b.cut ? b.b_clip_id : 0;
    selected_transition_.in_edge = !b.cut && b.in_edge;
    selected_transition_.valid = true;
    // Light the selected pill up so the Delete target is visible.
    for (auto& bb : transition_bubbles_) {
        if (!bb.pill) continue;
        const bool sel = bb.clip_id == selected_transition_.a &&
                         (bb.cut
                              ? bb.b_clip_id == selected_transition_.b
                              : bb.in_edge == selected_transition_.in_edge);
        bb.pill->setPen(sel ? QPen(QColor(0xFF, 0xD7, 0x4A, 255), 2.0)
                            : QPen(QColor(255, 255, 255, 230), 1.0));
        bb.pill->setZValue(sel ? 52 : 50);
    }
}

void TimelineWidget::clear_selected_transition() {
    selected_transition_ = {};
    for (auto& bb : transition_bubbles_) {
        if (bb.pill) bb.pill->setPen(QPen(QColor(255, 255, 255, 230), 1.0));
    }
}

bool TimelineWidget::delete_selected_transition() {
    if (!selected_transition_.valid || !sequence_) return false;
    const canvas::core::Clip* a = nullptr;
    for (const auto& t : sequence_->video_tracks)
        if ((a = t.clip_with_id(selected_transition_.a))) break;
    if (!a)
        for (const auto& t : sequence_->audio_tracks)
            if ((a = t.clip_with_id(selected_transition_.a))) break;
    if (!a) {
        // Stale selection (the clip was removed elsewhere): clear it and let
        // Delete fall through to normal clip behaviour.
        clear_selected_transition();
        return false;
    }

    const canvas::core::Clip* b = nullptr;
    if (selected_transition_.b != 0) {
        for (const auto& t : sequence_->video_tracks)
            if ((b = t.clip_with_id(selected_transition_.b))) break;
        if (!b)
            for (const auto& t : sequence_->audio_tracks)
                if ((b = t.clip_with_id(selected_transition_.b))) break;
    }
    const bool in_edge = selected_transition_.in_edge;
    clear_selected_transition();
    emit delete_transition_requested(a, b, in_edge);
    return true;
}

void TimelineWidget::open_transition_editor_for_cut(const canvas::core::Clip* a,
                                                    const canvas::core::Clip* b,
                                                    int64_t cut_frame) {
    // Cut transition spanning two abutting clips: a single bubble centered on the
    // edit point, seeded from whichever side carries the transition.
    if (!a || !b || !sequence_) return;
    const int v_count = static_cast<int>(sequence_->video_tracks.size());
    int flat = -1;
    canvas::core::Track::Kind kind = canvas::core::Track::Kind::Video;
    for (int i = 0; i < v_count; ++i) {
        if (sequence_->video_tracks[i].clip_with_id(a->id)) {
            flat = i;
            break;
        }
    }
    if (flat < 0) {
        const int a_count = static_cast<int>(sequence_->audio_tracks.size());
        for (int i = 0; i < a_count; ++i) {
            if (sequence_->audio_tracks[i].clip_with_id(a->id)) {
                flat = v_count + i;
                kind = canvas::core::Track::Kind::Audio;
                break;
            }
        }
    }
    if (flat < 0) return;

    transition_target_ = CutTarget{a, b, kind, flat, cut_frame, Edge::Cut};
    const int64_t seeded =
        a->transition_out_duration >= kMinTransitionFrames
            ? a->transition_out_duration
            : (b->transition_in_duration >= kMinTransitionFrames
                   ? b->transition_in_duration
                   : 6);
    const int64_t init = std::clamp(seeded, kMinTransitionFrames,
                                    transition_max_duration(transition_target_));
    transition_handle_duration_ = init;
    transition_handle_left_frame_ = cut_frame - init / 2;
    transition_handle_right_frame_ = cut_frame + (init - init / 2);
    transition_handle_visible_ = true;
    transition_snap_presets_ = false;
    rebuild_transition_handle();
}

void TimelineWidget::apply_selection_highlight() {
    int highlighted = 0;
    for (auto& item : clip_items_) {
        if (!item.outline) continue;
        const bool sel = item.clip && selection_.contains(item.clip->id);
        const double w = sel ? kClipSelectedOutlineW : kClipOutlineW;
        item.outline->setPen(sel ? QPen(QColor(0xD1, 0x5A, 0x3A), w)
                                 : QPen(QColor(0x4C, 0x92, 0xFF), w));
        // Keep the stroke's outer edge exactly on the clip's own boundary by
        // re-insetting the path as the pen width changes; a centred pen would
        // otherwise overhang into an abutting neighbour.
        const QRectF r = item.rect->rect();
        item.outline->setPath(rounded_rect_path(
            QRectF(w / 2.0, w / 2.0, r.width() - w, r.height() - w), 6));
        if (sel) highlighted++;
    }
    if (debug_enabled())
        qDebug() << "timeline: apply_selection_highlight matched" << highlighted
                 << "shell(s); selected ids ->["
                 << ([&] {
                        QString s;
                        for (const auto id : selection_.ids())
                            s += QString::number(static_cast<quint64>(id)) + QLatin1Char(' ');
                        return s;
                    }())
                 << "]";
}

void TimelineWidget::set_selection(const std::vector<canvas::core::ClipId>& ids) {
    selection_.set(ids, sequence_);
    if (debug_enabled()) {
        QString s;
        for (const auto id : selection_.ids())
            s += QString::number(static_cast<quint64>(id)) + QLatin1Char(' ');
        qDebug() << "timeline: selection set (with mates) ->" << s;
    }
    apply_selection_highlight();
}

void TimelineWidget::clear_selection() {
    selection_.clear();
    apply_selection_highlight();
}

void TimelineWidget::mousePressEvent(QMouseEvent* event) {
    const QPointF scene_pos = mapToScene(event->pos());

    // The playhead follows clicks/drags on the time ruler, the minimap, and the
    // playhead line itself; clicking clips must not move it. The ruler + minimap
    // are PINNED to the top of the viewport, so these tests use viewport coords.
    const double ruler_top = kMinimapHeight + kSceneMargin + kTimecodeBarHeight;
    const int vy = event->pos().y();
    // The whole pinned strip (timecode bar + minimap + ruler) seeks on a click,
    // so any press above the ruler's bottom edge counts as a ruler scrub.
    const bool in_ruler = vy < ruler_top + kRulerHeight;

    // Grabbing the playhead line itself (anywhere along its height, below the
    // pinned strips) lets you drag to scrub. The line is a full-height scene
    // item, so its X stays compared in scene coords.
    const double playhead_x = kSceneMargin + kTrackHeaderWidth + playhead_frame_ / frames_per_pixel_;
    constexpr double kPlayheadGrabPx = 5.0;
    const bool on_playhead =
        vy > kTimecodeBarHeight + kSceneMargin + kMinimapHeight &&
        std::abs(scene_pos.x() - playhead_x) <= kPlayheadGrabPx;

    const bool left = event->button() == Qt::LeftButton;
    const bool middle = event->button() == Qt::MiddleButton;
    if ((left || middle) && in_ruler) {
        emit playhead_moved(frame_at_x(event->pos().x()));
        set_playhead_position(frame_at_x(event->pos().x()));
    }

    const bool on_minimap = vy >= kTimecodeBarHeight + kSceneMargin &&
                            vy <= kTimecodeBarHeight + kSceneMargin + kMinimapHeight &&
                            scene_pos.x() > kSceneMargin + kTrackHeaderWidth;

    if (on_minimap && sequence_) {
        const double strip_w = scene_.sceneRect().width() - kTrackHeaderWidth;
        const double dur = std::max<int64_t>(sequence_->duration_frames(), 1);
        if (strip_w > 0) {
            const int64_t frame = std::max<int64_t>(
                0, static_cast<int64_t>((scene_pos.x() - kSceneMargin - kTrackHeaderWidth) *
                                        dur / strip_w));
            set_playhead_position(frame);
            emit playhead_moved(frame);
        }
        event->accept();
        return;
    }

    if (event->button() != Qt::LeftButton) {
        QGraphicsView::mousePressEvent(event);
        return;
    }

    // Track-header row edges are live resize handles: each boundary reallocates
    // the rows it separates, while the spacer dividers above/below the stack
    // grow/shrink the empty padding. This must beat the header-strip marquee,
    // so it runs before range-select arming.
    //
    // The Video/Audio divider is its OWN fixed band, separated from V1 above
    // and A1 below: its top edge resizes V1, its bottom edge resizes A1, and
    // the band's interior is a grab-and-scroll pan handle that moves the ENTIRE
    // timeline as one unit — never resizing anything.
    if (sequence_ && has_timeline_content()) {
        const int v_count = static_cast<int>(sequence_->video_tracks.size());
        const int a_count = static_cast<int>(sequence_->audio_tracks.size());
        const bool in_header_strip = scene_pos.x() < kSceneMargin + kTrackHeaderWidth;
        const int edge = header_resize_target(scene_pos.y(), v_count, a_count);
        // The divider band's flanking edges (V1's bottom, A1's top) act as row
        // resize handles along their ENTIRE width, so they work from the
        // timeline body too. The band's interior is pan.
        const bool on_band_edge = edge == v_count || edge == v_count + 1;
        const bool grab_pan = in_section_divider_band(scene_pos.y(), v_count, a_count) && edge < 0;
        const bool grab_row = !grab_pan && edge >= 0 && (in_header_strip || on_band_edge);
        if (debug_enabled())
            qDebug() << "timeline: header press x=" << scene_pos.x() << "y=" << scene_pos.y()
                     << "in_header_strip=" << in_header_strip << "edge=" << edge
                     << "v=" << v_count << "a=" << a_count
                     << "grab_pan=" << grab_pan << "grab_row=" << grab_row;
        if (grab_pan) {
            // Divider band interior = vertical pan handle: drags the whole timeline
            // (rows + header column) with the cursor, no resizing.
            pan_dragging_ = true;
            pan_anchor_viewport_y_ = event->pos().y();
            pan_start_scroll_ = verticalScrollBar() ? verticalScrollBar()->value() : 0;
            if (debug_enabled())
                qDebug() << "timeline: pan press anchor_y=" << pan_anchor_viewport_y_
                         << "start_scroll=" << pan_start_scroll_;
            setCursor(Qt::SizeVerCursor);
            track_resize_cursor_shown_ = true;
            event->accept();
            return;
        }
        if (grab_row) {
            const int band = (v_count > 0 && a_count > 0) ? 1 : 0;
            const int total = v_count + a_count + band;
            resizing_track_ = true;
            resize_edge_ = edge;
            resize_total_ = total;
            resize_start_y_ = edge_y(edge, v_count, a_count);
            if (edge == 0) {
                resize_above_start_ = 0.0;
                resize_below_start_ = track_v_pad_top_;
            } else if (edge == total) {
                resize_above_start_ = track_v_pad_bottom_;
                resize_below_start_ = 0.0;
            } else {
                resize_above_start_ =
                    track_height(flat_of_screen_row(edge - 1, v_count), v_count);
                resize_below_start_ =
                    track_height(flat_of_screen_row(edge, v_count), v_count);
            }
            setCursor(Qt::SizeVerCursor);
            track_resize_cursor_shown_ = true;
            event->accept();
            return;
        }
    }

    // Transition cut-handle: grabbing a resize edge takes priority over a normal
    // clip drag. A persistent bubble grab works the same way (and snaps to the
    // preset durations while dragging).
    if (maybe_press_transition_bubble(scene_pos)) {
        event->accept();
        return;
    }
    press_transition_handle(scene_pos);
    if (transition_handle_dragging_) {
        event->accept();
        return;
    }

    // A left-click that isn't on a transition bubble deselects any selected
    // transition, so Delete returns to normal clip behaviour.
    transition_press_armed_ = false;
    if (selected_transition_.valid) clear_selected_transition();

    ClipItem* hit = nullptr;
    std::vector<ClipItem*> candidates;
    for (auto& item : clip_items_) {
        if (item.rect && item.rect->sceneBoundingRect().contains(scene_pos)) {
            if (!hit) hit = &item;
            candidates.push_back(&item);
        }
    }

    // Direct hit-tests can miss the thin sliver at the bottom of each track and
    // the kTrackGap between tracks. Treat the whole row as clickable: if the
    // direct test found nothing, pick the top-most clip whose frame range
    // contains the click, on the track under the cursor.
    bool fallback_attempted = false;
    bool fallback_hit = false;
    if (!hit && sequence_ && scene_pos.x() >= kSceneMargin + kTrackHeaderWidth) {
        const int v_count = static_cast<int>(sequence_->video_tracks.size());
        const int total = v_count + static_cast<int>(sequence_->audio_tracks.size());
        const int clicked_track = track_at_y(scene_pos.y(), v_count);
        const int64_t frame = frame_at_x(event->pos().x());
        fallback_attempted = true;
        if (debug_enabled())
            qDebug() << "timeline: hit-test direct miss -> fallback track_at_y=" << clicked_track
                     << "scene_y=" << scene_pos.y() << "frame=" << frame;
        if (clicked_track >= 0 && clicked_track < total) {
            ClipItem* best = nullptr;
            for (auto& item : clip_items_) {
                if (item.track_index != clicked_track) continue;
                if (!item.clip) continue;
                if (frame >= item.clip->tl_in && frame < item.clip->tl_out) {
                    if (!best || item.rect->sceneBoundingRect().top() < best->rect->sceneBoundingRect().top())
                        best = &item;
                }
            }
            if (best) {
                hit = best;
                candidates.push_back(best);
                fallback_hit = true;
            }
        }
    }

    if (debug_enabled()) {
        std::string cands;
        for (const auto* c : candidates)
            cands += (cands.empty() ? "" : ";") + std::to_string(c->clip->id) + "[" +
                     std::to_string(c->clip->tl_in) + "," + std::to_string(c->clip->tl_out) + "]";
        qDebug() << "timeline: hit-test scene_x=" << scene_pos.x()
                 << "y=" << scene_pos.y() << "frame=" << frame_at_x(event->pos().x())
                 << "tool=" << (current_tool_ == Tool::Blade ? "blade"
                             : current_tool_ == Tool::Select ? "select" : "other")
                 << "direct_hit=" << (hit && !fallback_attempted ? "yes" : "no")
                 << "fallback_attempted=" << (fallback_attempted ? "yes" : "no")
                 << "fallback_hit=" << (fallback_hit ? "yes" : "no")
                 << "hit_id=" << (hit ? static_cast<long long>(hit->clip->id) : -1)
                 << "candidates=[" << QString::fromStdString(cands) << "]";
    }

    if (hit && current_tool_ == Tool::Blade) {
        if (debug_enabled())
            qDebug() << "timeline: blade click clip id=" << hit->clip->id
                     << "at frame" << frame_at_x(event->pos().x());
        emit blade_requested(hit->clip, frame_at_x(event->pos().x()));
        event->accept();
        return;
    }

    // The track-header strip is chrome: a plain click must never clear the clip
    // selection or start a drag. Dragging FROM the strip starts a marquee — the
    // strip is always empty, so it is a reliable handle for sweeping a selection
    // (a click that never travels emits no range, so it stays inert).
    if (scene_pos.x() < kSceneMargin + kTrackHeaderWidth) {
        if (!in_ruler && !on_playhead) {
            is_selecting_range_ = true;
            marquee_full_height_ = true;
            drag_scene_start_ = scene_pos;
            drag_start_frame_ = 0;
            drag_start_pos_ = event->pos();
            if (!selection_rect_) {
                selection_rect_ = scene_.addRect(QRectF(), QPen(QColor(0x4C, 0x92, 0xFF)),
                                                 QBrush(QColor(0x3B, 0x82, 0xF6, 40)));
                selection_rect_->setZValue(90);
            }
            const double x0 = kSceneMargin + kTrackHeaderWidth;
            selection_rect_->setRect(QRectF(x0, kSceneMargin, 0, scene_.sceneRect().height()));
        }
        event->accept();
        return;
    }

    if (!hit) {
        if (debug_enabled())
            qDebug() << "timeline: select: NO CLIP HIT at frame"
                     << frame_at_x(event->pos().x()) << "scene_y=" << scene_pos.y()
                     << "(starts range-select/scrub, did not select a clip)";
        // Clicking/dragging on the ruler, or grabbing the playhead line, scrubs
        // through the footage instead of starting a range select.
        if (in_ruler || on_playhead) {
            is_scrubbing_ = true;
            drag_start_pos_ = event->pos();
            scrub_to_frame(frame_at_x(event->pos().x()));
            event->accept();
            return;
        }
        clear_selection();
        is_selecting_range_ = true;
        marquee_full_height_ = false;
        drag_scene_start_ = scene_pos;
        drag_start_frame_ = frame_at_x(event->pos().x());
        drag_start_pos_ = event->pos();
        if (!selection_rect_) {
            selection_rect_ = scene_.addRect(QRectF(), QPen(QColor(0x4C, 0x92, 0xFF)),
                                             QBrush(QColor(0x3B, 0x82, 0xF6, 40)));
            selection_rect_->setZValue(90);
        }
        const double x0 = kSceneMargin + kTrackHeaderWidth + drag_start_frame_ / frames_per_pixel_;
        selection_rect_->setRect(QRectF(x0, kSceneMargin, 0, scene_.sceneRect().height()));
        event->accept();
        return;
    }

    emit clip_selected(hit->clip);
    if (debug_enabled())
        qDebug() << "timeline: click clip id=" << hit->clip->id
                 << "tl=" << hit->clip->tl_in << "-" << hit->clip->tl_out
                 << "linked=" << hit->clip->linked_id;
    const auto mates =
        sequence_ ? timeline_selection::expand_with_mates({hit->clip->id}, *sequence_)
                  : std::vector<canvas::core::ClipId>{hit->clip->id};
    if (event->modifiers() & Qt::ControlModifier) {
        // Ctrl+click toggles the clip (and its mate) in/out of the selection.
        bool already = selection_.contains(hit->clip->id);
        auto kept = selection_.ids();
        if (already) {
            for (const auto id : mates)
                kept.erase(std::remove(kept.begin(), kept.end(), id), kept.end());
        } else {
            for (const auto id : mates)
                if (std::find(kept.begin(), kept.end(), id) == kept.end())
                    kept.push_back(id);
        }
        set_selection(kept);
    } else if (event->modifiers() & Qt::ShiftModifier) {
        // Shift+click adds the clip (and its mate) to the selection.
        auto acc = selection_.ids();
        for (const auto id : mates)
            if (std::find(acc.begin(), acc.end(), id) == acc.end())
                acc.push_back(id);
        set_selection(acc);
    } else {
        // Plain click selects just this clip and its linked mate.
        set_selection({hit->clip->id});
    }
    if (debug_enabled()) {
        QString sel;
        for (const auto id : selection_.ids())
            sel += QString::number(static_cast<quint64>(id)) + QLatin1Char(' ');
        qDebug() << "timeline: select: SUCCESS after 1 click. clip id="
                 << static_cast<long long>(hit->clip->id)
                 << "selected set ->" << sel;
    }
    dragged_clip_ = hit;
    original_track_index_ = hit->track_index;
    drag_mate_ = find_linked_mate(hit);
    drag_press_pos_ = event->pos();
    drag_start_frame_ = frame_at_x(event->pos().x());
    drag_grab_offset_px_ = scene_pos.x() - hit->rect->rect().left();
    drag_ctrl_.begin(frame_at_x(event->pos().x()), hit->clip->tl_in, hit->track_index);
    // Selection happens on press; the move only engages once the pointer actually
    // travels past the drag threshold, so a plain click can never nudge the clip.
    is_dragging_ = false;
    event->accept();
}

void TimelineWidget::mouseMoveEvent(QMouseEvent* event) {
    // Video/Audio section-divider PAN (grab-and-follow): the rows move WITH the
    // hand, up to their normal seat under the ruler strip (the UP limit) or
    // down into the empty room below — pure scroll, no resizing.
    if (pan_dragging_) {
        const double dy = event->pos().y() - pan_anchor_viewport_y_;
        if (auto* bar = verticalScrollBar()) {
            const int before = bar->value();
            const int target = static_cast<int>(lround(pan_start_scroll_ - dy));
            // Clamp between the top of the scene (fully sunk) and the parked position
            // (channels seated under the ruler strip).
            bar->setValue(std::clamp(target, 0, bar->maximum()));
            if (debug_enabled())
                qDebug() << "timeline: pan move dy=" << dy << "scroll" << before << "->"
                         << bar->value() << "sceneTopAtViewport=" << mapToScene(0, 0).y();
        }
        event->accept();
        return;
    }

    // Live track-row resize drag (pressed on a header boundary): the grabbed
    // divider follows the cursor — an interior edge reallocates the two rows it
    // separates, while the stack's outer edges grow/shrink the empty padding.
    if (resizing_track_) {
        const int v_count = sequence_ ? static_cast<int>(sequence_->video_tracks.size()) : 0;
        const int a_count = sequence_ ? static_cast<int>(sequence_->audio_tracks.size()) : 0;
        const QPointF sp = mapToScene(event->pos());
        const double dy = sp.y() - resize_start_y_;   // positive = dragged down
        if (resize_edge_ == 0) {
            track_v_pad_top_ =
                std::clamp(resize_below_start_ + dy, kMinTrackVPad, kMaxTrackVPad);
        } else if (resize_edge_ >= resize_total_) {
            track_v_pad_bottom_ =
                std::clamp(resize_above_start_ + dy, kMinTrackVPad, kMaxTrackVPad);
        } else {
            // Interior edge: the boundary sits at the bottom of the row above, so it
            // follows the cursor only when the ABOVE row grows on a downward
            // drag and the BELOW row shrinks by the same space. The divider
            // band's own edges resize only the row on one side of the band
            // (top edge = V1, bottom edge = A1); the other side maps to the
            // fixed band (flat_of_screen_row -1) and is skipped.
            const int up_flat = flat_of_screen_row(resize_edge_ - 1, v_count);
            const int dn_flat = flat_of_screen_row(resize_edge_, v_count);
            if (up_flat >= 0) set_track_height(up_flat, v_count, resize_above_start_ + dy);
            if (dn_flat >= 0) set_track_height(dn_flat, v_count, resize_below_start_ - dy);
        }
        rebuild_timeline();
        event->accept();
        return;
    }

    if (is_scrubbing_) {
        scrub_to_frame(frame_at_x(event->pos().x()));
        event->accept();
        return;
    }

    // A transition bubble was pressed: the drag (and its editor overlay) only
    // engages once the pointer actually travels, so a plain click just selects.
    if (transition_press_armed_) {
        const QPointF sp = mapToScene(event->pos());
        if ((sp - transition_press_pos_).manhattanLength() >=
            QApplication::startDragDistance()) {
            engage_transition_drag(sp);
        } else {
            event->accept();
            return;
        }
    }

    // Transition resize drag in progress: update it and bail so it never
    // competes with clip/scrub drags.
    if (transition_handle_dragging_) {
        move_transition_handle(mapToScene(event->pos()));
        event->accept();
        return;
    }

    // Hover feedback for the track-header row-resize dividers: a vertical-size
    // cursor appears over any row edge while the pointer is free.
    if (!is_selecting_range_ && !is_dragging_ && !dragged_clip_ &&
        !transition_press_armed_ && !resizing_track_) {
        const QPointF sp = mapToScene(event->pos());
        bool hovering_edge = false;
        if (sequence_) {
            const int v_count = static_cast<int>(sequence_->video_tracks.size());
            const int a_count = static_cast<int>(sequence_->audio_tracks.size());
            const int edge = header_resize_target(sp.y(), v_count, a_count);
            // The divider band's interior (pan handle) and its two flanking edges
            // (V1 / A1 resize handles) are all vertical-drag targets, in the
            // header column and across the whole timeline body.
            const bool in_band =
                in_section_divider_band(sp.y(), v_count, a_count);
            if (sp.x() < kSceneMargin + kTrackHeaderWidth) {
                // Header column: every row edge is a live resize handle, plus
                // the divider band's pan interior.
                hovering_edge = edge >= 0 || in_band;
            } else if (v_count > 0 && a_count > 0) {
                // Timeline body: only the divider band (edges + interior).
                hovering_edge = edge == v_count || edge == v_count + 1 || in_band;
            }
        }
        if (hovering_edge) {
            if (!track_resize_cursor_shown_) {
                setCursor(Qt::SizeVerCursor);
                track_resize_cursor_shown_ = true;
            }
        } else if (track_resize_cursor_shown_) {
            unsetCursor();
            track_resize_cursor_shown_ = false;
        }
    }

    // A clip was pressed but the move hasn't engaged yet: absorb wiggles under
    // the platform drag threshold so a plain click never turns into a move.
    if (dragged_clip_ && !is_dragging_ && !is_selecting_range_) {
        if ((event->pos() - drag_press_pos_).manhattanLength() >= QApplication::startDragDistance()) {
            is_dragging_ = true;
        } else {
            event->accept();
            return;
        }
    }

    // Instant hover preview of a cut / single-clip edge: a ghost overlay shows
    // the would-be transition region. Visual ONLY — press_transition_handle
    // refuses to start an edit unless a transition ALREADY exists, so a stray
    // press+drag can never create one (adding is right-click only).
    if (current_tool_ == Tool::Select && !is_dragging_ && !is_selecting_range_ &&
        !transition_press_armed_ && !transition_handle_dragging_) {
        update_transition_hover(mapToScene(event->pos()));
    }

    if (current_tool_ == Tool::Blade && !is_dragging_ && !is_selecting_range_) {
        update_blade_preview(frame_at_x(event->pos().x()));
    }

    if (is_selecting_range_) {
        const QPointF cur = mapToScene(event->pos());
        const double left = std::max(static_cast<double>(kSceneMargin + kTrackHeaderWidth),
                                     std::min(drag_scene_start_.x(), cur.x()));
        const double right = std::max(drag_scene_start_.x(), cur.x());
        double top, bottom;
        if (marquee_full_height_ && sequence_) {
            const int v_count = static_cast<int>(sequence_->video_tracks.size());
            const int a_count = static_cast<int>(sequence_->audio_tracks.size());
            top    = tracks_stack_top();
            bottom = tracks_stack_bottom(v_count, a_count);
        } else {
            top    = std::min(drag_scene_start_.y(), cur.y());
            bottom = std::max(drag_scene_start_.y(), cur.y());
        }
        selection_rect_->setRect(QRectF(QPointF(left, top), QPointF(right, bottom)));
        event->accept();
        return;
    }

    if (is_dragging_ && dragged_clip_) {
        const int v_count = sequence_ ? static_cast<int>(sequence_->video_tracks.size()) : 1;
        const double scene_y = mapToScene(event->pos()).y();
        const int candidate = track_at_y(scene_y, v_count);

        // Dragged a video clip up past the top video track: auto-create a new upper
        // video channel (plus an audio channel for a linked mate) and hand the
        // clip over to it, in one undoable model edit.
        if (dragged_clip_->track_kind == canvas::core::Track::Kind::Video && !promote_latched_ &&
            sequence_ && candidate < 0 &&
            scene_y < static_cast<double>(track_top(0, v_count))) {
            const int64_t snap_tl =
                drag_ctrl_.move(frame_at_x(event->pos().x()), candidate, v_count,
                                dragged_clip_->track_kind, snap_enabled_ && fps_ > 0.0,
                                frames_per_pixel_)
                    .new_tl_in;
            promote_latched_ = true;
            const canvas::core::ClipId mate_id =
                drag_mate_ && drag_mate_->clip ? drag_mate_->clip->id : 0;
            emit new_upper_track_requested(dragged_clip_->clip->id, snap_tl);
            // The handler did a synchronous rebuild; re-acquire the clip (and its mate)
            // by id and resume dragging on the new track. If the clip vanished
            // from the model, abandon the drag cleanly.
            if (!reacquire_dragged_clip(dragged_clip_->clip->id, mate_id,
                                        frame_at_x(event->pos().x()))) {
                is_dragging_ = false;
                drag_ctrl_.end();
                dragged_clip_ = nullptr;
                drag_mate_ = nullptr;
                event->accept();
                return;
            }
        }

        const auto res = drag_ctrl_.move(
            frame_at_x(event->pos().x()), candidate, v_count, dragged_clip_->track_kind,
            snap_enabled_ && fps_ > 0.0, frames_per_pixel_);

        // The controller resolved the same-kind target track under the cursor;
        // paint the primary clip on it (the mate keeps its own lane).
        const int target = drag_ctrl_.target_track_index();
        if (target >= 0) {
            dragged_clip_->track_kind =
                target < v_count ? canvas::core::Track::Kind::Video : canvas::core::Track::Kind::Audio;
            dragged_clip_->track_index = target;
        }

        position_clip_at(*dragged_clip_, res.new_tl_in);
        if (drag_mate_) position_clip_at(*drag_mate_, res.new_tl_in);
        update_snap_indicator(res.snapped, res.new_tl_in);
        event->accept();
        return;
    }

    QGraphicsView::mouseMoveEvent(event);
}

void TimelineWidget::mouseReleaseEvent(QMouseEvent* event) {
    // End a section-divider pan; the scroll offset is already live.
    if (pan_dragging_) {
        pan_dragging_ = false;
        event->accept();
        return;
    }

    // End a track-row resize drag; the heights/paddings are already updated live.
    if (resizing_track_) {
        resizing_track_ = false;
        resize_edge_ = -1;
        resize_total_ = 0;
        event->accept();
        return;
    }

    // A bubble was clicked but never dragged into a resize: plain click, the
    // transition stays selected (Delete removes it) and nothing was edited.
    if (transition_press_armed_) {
        transition_press_armed_ = false;
        event->accept();
        return;
    }

    // Finish a transition resize drag, committing the new duration.
    if (transition_handle_dragging_) {
        release_transition_handle();
        event->accept();
        return;
    }

    if (is_scrubbing_) {
        if (debug_enabled())
            qDebug() << "timeline: scrub released at frame" << frame_at_x(event->pos().x());
        const int64_t settled = frame_at_x(event->pos().x());
        is_scrubbing_ = false;
        emit playhead_committed(settled);
        event->accept();
        return;
    }

    if (is_selecting_range_) {
        const int64_t end = frame_at_x(event->pos().x());
        const int64_t lo = std::max<int64_t>(0, std::min(drag_start_frame_, end));
        const int64_t hi = std::max(drag_start_frame_, end);
        if (debug_enabled())
            qDebug() << "timeline: range select" << lo << "-" << hi;
        if (hi > lo) {
            std::vector<canvas::core::ClipId> ids;
            if (sequence_) {
                const int v_count = static_cast<int>(sequence_->video_tracks.size());
                const int total = v_count + static_cast<int>(sequence_->audio_tracks.size());
                int min_track, max_track;
                if (marquee_full_height_) {
                    min_track = 0;
                    max_track = total - 1;
                } else {
                    const QPointF cur = mapToScene(event->pos());
                    const double y_min = std::min(drag_scene_start_.y(), cur.y());
                    const double y_max = std::max(drag_scene_start_.y(), cur.y());
                    min_track = total;
                    max_track = -1;
                    for (int i = 0; i < total; ++i) {
                        const double t = track_top(i, v_count);
                        if (t + track_height(i, v_count) > y_min && t < y_max) {
                            min_track = std::min(min_track, i);
                            max_track = std::max(max_track, i);
                        }
                    }
                }
                ids = timeline_selection::clips_in_range_and_tracks(*sequence_, lo, hi,
                                                                    min_track, max_track);
            }
            emit range_selected(lo, hi);
            emit clips_range_selected(ids);
        }
        if (selection_rect_) {
            scene_.removeItem(selection_rect_);
            selection_rect_ = nullptr;
        }
        is_selecting_range_ = false;
        marquee_full_height_ = false;
        event->accept();
        return;
    }

    if (is_dragging_ && dragged_clip_) {
        update_snap_indicator(false, dragged_clip_->clip->tl_in);
        const int v_count = sequence_ ? static_cast<int>(sequence_->video_tracks.size()) : 1;
        const auto res = drag_ctrl_.commit(
            frame_at_x(event->pos().x()), dragged_clip_->clip->tl_in,
            snap_enabled_ && fps_ > 0.0, frames_per_pixel_);
        if (res.changed) {
            const canvas::core::Track::Kind kind = dragged_clip_->track_kind;
            const int per_kind = kind_track_index(dragged_clip_->track_index, v_count);
            emit clip_moved(dragged_clip_->clip, res.new_tl_in, kind, per_kind);
        }
        is_dragging_ = false;
        drag_ctrl_.end();
        dragged_clip_ = nullptr;
        drag_mate_ = nullptr;
        event->accept();
        return;
    }

    // A clip was pressed but the move never engaged (click below the drag
    // threshold): the selection already happened on press, so just release the
    // grab state and never emit a move.
    if (dragged_clip_) {
        is_dragging_ = false;
        drag_ctrl_.end();
        dragged_clip_ = nullptr;
        drag_mate_ = nullptr;
        update_snap_indicator(false, 0);
        event->accept();
        return;
    }

    QGraphicsView::mouseReleaseEvent(event);
}

void TimelineWidget::dragEnterEvent(QDragEnterEvent* event) {
    if (event->mimeData()->hasFormat("application/x-eh-media-id") ||
        event->mimeData()->hasUrls()) {
        event->acceptProposedAction();
    } else {
        event->ignore();
    }
}

void TimelineWidget::dragMoveEvent(QDragMoveEvent* event) {
    if (event->mimeData()->hasFormat("application/x-eh-media-id") ||
        event->mimeData()->hasUrls()) {
        event->acceptProposedAction();
    } else {
        event->ignore();
    }
}

void TimelineWidget::dropEvent(QDropEvent* event) {
    if (event->mimeData()->hasFormat("application/x-eh-media-id")) {
        const int media_id = event->mimeData()->data("application/x-eh-media-id").toInt();
        const int64_t frame = frame_at_x(event->position().toPoint().x());
        emit media_dropped(media_id, frame);
        event->acceptProposedAction();
        return;
    }

    if (event->mimeData()->hasUrls()) {
        QStringList paths;
        const auto urls = event->mimeData()->urls();
        for (const QUrl& url : urls) {
            if (url.isLocalFile()) paths.append(url.toLocalFile());
        }
        if (!paths.isEmpty()) {
            const int64_t frame = frame_at_x(event->position().toPoint().x());
            emit media_files_dropped(paths, frame);
        }
        event->acceptProposedAction();
        return;
    }

    event->ignore();
}

}  // namespace canvas::gui
