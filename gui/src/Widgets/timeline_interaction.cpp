#include "Widgets/timeline_widget.hpp"
#include "Widgets/timeline_snap.hpp"
#include "features/timeline/audio_targets.hpp"
#include "Logging.hpp"
#include "UX/theme.hpp"
#include "core/timecode.hpp"

#include <QApplication>
#include <QCursor>
#include <QColor>
#include <QDateTime>
#include <QMouseEvent>
#include <QPen>
#include <QBrush>
#include <QPointF>
#include <QRectF>
#include <QGraphicsItem>
#include <QGraphicsItemGroup>
#include <QGraphicsRectItem>
#include <QGraphicsSimpleTextItem>
#include <QString>
#include <QToolTip>

#include <chrono>
#include <cstdlib>
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
#include <QFontMetricsF>
#include <QLineF>
#include <QDragEnterEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QMimeData>
#include <QUrl>

#include <algorithm>
#include <cmath>
#include <unordered_set>

#include "timeline_volume_line.hpp"

namespace canvas::gui {

int TimelineWidget::track_at_y(double scene_y, int v_count) const {
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

TimelineWidget::DropLane TimelineWidget::resolve_drop_lane(double scene_y,
                                                           canvas::core::Track::Kind media_kind) const {
    const int v_count = sequence_ ? static_cast<int>(sequence_->video_tracks.size()) : 0;
    const int a_count = sequence_ ? static_cast<int>(sequence_->audio_tracks.size()) : 0;
    const bool base_populated =
        v_count > 0 && a_count > 0 &&
        !sequence_->video_tracks[0].clips.empty() &&
        !sequence_->audio_tracks[0].clips.empty();
    if (!base_populated) return {media_kind, 0};
    if (media_kind == canvas::core::Track::Kind::Video) {
        for (int f = v_count - 1; f >= 0; --f) {
            const double t = track_top(f, v_count);
            if (scene_y >= t && scene_y < t + track_height(f, v_count))
                return {media_kind, f};
        }
        if (v_count == 0) return {media_kind, 0};
        if (scene_y < track_top(v_count - 1, v_count)) return {media_kind, v_count};
        return {media_kind, 0};
    }
    for (int f = 0; f < a_count; ++f) {
        const double t = track_top(v_count + f, v_count);
        if (scene_y >= t && scene_y < t + track_height(v_count + f, v_count))
            return {media_kind, f};
    }
    if (a_count == 0) return {media_kind, 0};
    const double last_bottom = track_top(v_count + a_count - 1, v_count) +
                               track_height(v_count + a_count - 1, v_count);
    if (scene_y >= last_bottom) return {media_kind, a_count};
    return {media_kind, 0};
}

int TimelineWidget::flat_row_at_scene_y(double scene_y) const {
    if (!sequence_ || !has_timeline_content()) return -1;
    const int v_count = static_cast<int>(sequence_->video_tracks.size());
    const int a_count = static_cast<int>(sequence_->audio_tracks.size());
    if (in_section_divider_band(scene_y, v_count, a_count)) return -1;
    for (int f = 0; f < v_count + a_count; ++f) {
        const double t = track_top(f, v_count);
        if (scene_y >= t && scene_y < t + track_height(f, v_count)) return f;
    }
    return -1;
}

QGraphicsRectItem* TimelineWidget::highlight_scene_item(QGraphicsRectItem*& slot) {
    if (!slot) {
        slot = scene_.addRect(QRectF(), QPen(Qt::NoPen), QBrush(Qt::NoBrush));
        slot->setAcceptedMouseButtons(Qt::NoButton);
        slot->setZValue(35);
    }
    return slot;
}

void TimelineWidget::set_rect_highlight(QGraphicsRectItem*& slot, const QRectF& rect,
                                        const QBrush& fill, const QPen& pen) {
    QGraphicsRectItem* it = highlight_scene_item(slot);
    it->setRect(rect);
    it->setBrush(fill);
    it->setPen(pen);
}

void TimelineWidget::set_row_highlight(QGraphicsRectItem*& slot, int flat,
                                       const QBrush& fill, const QPen& pen) {
    if (flat < 0 || !has_timeline_content()) return;
    const int v_count = static_cast<int>(sequence_->video_tracks.size());
    set_rect_highlight(slot, QRectF(kSceneMargin, track_top(flat, v_count),
                                    scene_.sceneRect().right() - kSceneMargin,
                                    track_height(flat, v_count)),
                       fill, pen);
}

void TimelineWidget::clear_row_highlight(QGraphicsRectItem*& slot, int& flat) {
    if (slot) {
        scene_.removeItem(slot);
        delete slot;
        slot = nullptr;
    }
    flat = -1;
}

void TimelineWidget::update_hover_row(const QPointF& scene_pos) {
    if (!hover_highlight_) hover_flat_ = -1;
    if (!has_timeline_content()) {
        if (hover_flat_ != -1) clear_row_highlight(hover_highlight_, hover_flat_);
        return;
    }
    const int flat = flat_row_at_scene_y(scene_pos.y());
    if (flat == hover_flat_) return;
    hover_flat_ = flat;
    if (flat >= 0) {
        const ThemeTokens& t = tokens();
        QColor fill = t.state_hover;
        fill.setAlpha(90);
        set_row_highlight(hover_highlight_, flat, fill, QPen(Qt::NoPen));
    } else {
        clear_row_highlight(hover_highlight_, hover_flat_);
    }
}

void TimelineWidget::update_drop_lane(const QPointF& scene_pos) {
    if (!sequence_) return;
    if (!drop_lane_highlight_) drop_lane_flat_ = -1;
    const bool has_content = has_timeline_content();
    const bool base_populated =
        !sequence_->video_tracks.empty() && !sequence_->audio_tracks.empty() &&
        !sequence_->video_tracks[0].clips.empty() &&
        !sequence_->audio_tracks[0].clips.empty();
    const bool lane_precise = has_content && base_populated;
    const int flat = lane_precise ? flat_row_at_scene_y(scene_pos.y()) : -1;
    const int target = lane_precise ? flat : -2;
    if (target == drop_lane_flat_) return;
    drop_lane_flat_ = target;
    if (target == -2) {
        const ThemeTokens& t = tokens();
        QColor fill = t.accent;
        fill.setAlpha(40);
        set_rect_highlight(drop_lane_highlight_,
                           QRectF(kSceneMargin, empty_state_top(),
                                  scene_.sceneRect().right() - kSceneMargin, kEmptyStateHeight),
                           fill, QPen(t.accent, 2.0));
        return;
    }
    if (flat >= 0) {
        const ThemeTokens& t = tokens();
        QColor fill = t.accent;
        fill.setAlpha(26);
        set_row_highlight(drop_lane_highlight_, flat, fill, QPen(t.accent, 2.0));
    } else {
        clear_row_highlight(drop_lane_highlight_, drop_lane_flat_);
    }
}

void TimelineWidget::clear_drop_preview() {
    if (drop_overlay_) {
        scene_.removeItem(drop_overlay_);
        delete drop_overlay_;
        drop_overlay_ = nullptr;
    }
}

void TimelineWidget::update_drop_preview(const QPoint& widget_pos, int media_id) {
    if (!sequence_) {
        clear_drop_preview();
        return;
    }
    const auto it = media_paths_.find(static_cast<canvas::core::MediaId>(media_id));
    if (it == media_paths_.end() || it->second.total_frames <= 0) {
        clear_drop_preview();
        return;
    }
    const MediaMeta& meta = it->second;
    const int v_count = static_cast<int>(sequence_->video_tracks.size());
    const int a_count = static_cast<int>(sequence_->audio_tracks.size());
    const bool has_content = has_timeline_content();

    const auto lane =
        resolve_drop_lane(mapToScene(widget_pos).y(),
                          meta.is_video ? canvas::core::Track::Kind::Video
                                        : canvas::core::Track::Kind::Audio);
    const bool opens_new_track =
        has_content && (meta.is_video ? lane.index >= v_count : lane.index >= a_count);
    if (opens_new_track) {
        clear_drop_preview();
        return;
    }

    const ThemeTokens& t = tokens();
    const double content_w = scene_.sceneRect().right() - kSceneMargin;

    struct PanelRow {
        int flat;
        double top;
        double h;
    };
    std::vector<PanelRow> rows;
    rows.reserve(v_count + a_count);
    double stack_top = tracks_origin_y();
    double stack_bot = tracks_stack_bottom(v_count, a_count);
    if (has_content) {
        for (int f = 0; f < v_count + a_count; ++f)
            rows.push_back({f, track_top(f, v_count), track_height(f, v_count)});
    } else {
        stack_top = empty_state_top();
        std::vector<double> heights;
        for (int s = v_count - 1; s >= 0; --s) heights.push_back(track_height(s, v_count));
        if (v_count > 0 && a_count > 0) heights.push_back(kSectionDividerHeight);
        for (int s = 0; s < a_count; ++s) heights.push_back(track_height(v_count + s, v_count));
        double total = 0.0;
        for (double h : heights) total += h;
        total += kTrackGap * static_cast<double>(std::max(0, static_cast<int>(heights.size()) - 1));
        const double scale = total > kEmptyStateHeight ? kEmptyStateHeight / total : 1.0;
        std::vector<int> flats;
        flats.reserve(heights.size());
        for (int s = v_count - 1; s >= 0; --s) flats.push_back(s);
        if (v_count > 0 && a_count > 0) flats.push_back(-1);
        for (int s = 0; s < a_count; ++s) flats.push_back(v_count + s);
        double y = stack_top;
        for (size_t i = 0; i < flats.size(); ++i) {
            const double h = heights[i] * scale;
            if (flats[i] >= 0) rows.push_back({flats[i], y, h});
            y += h;
            if (i + 1 < flats.size()) y += kTrackGap * scale;
        }
        stack_bot = y;
    }

    const int target_flat = meta.is_video ? lane.index : v_count + lane.index;
    const auto target_row =
        std::find_if(rows.begin(), rows.end(),
                     [target_flat](const PanelRow& r) { return r.flat == target_flat; });
    if (target_row == rows.end()) {
        clear_drop_preview();
        return;
    }
    const double lane_top = target_row->top;
    const double lane_h = target_row->h;

    const int64_t frame = std::max<int64_t>(0, frame_at_x(widget_pos.x()));
    const double x = kSceneMargin + kTrackHeaderWidth + frame / frames_per_pixel_;
    const double w = std::max(2.0, static_cast<double>(meta.total_frames) / frames_per_pixel_);

    if (!drop_overlay_) {
        drop_overlay_ = new QGraphicsItemGroup;
        drop_overlay_->setAcceptedMouseButtons(Qt::NoButton);
        drop_overlay_->setZValue(38);
        scene_.addItem(drop_overlay_);
    }
    const QList<QGraphicsItem*> kids = drop_overlay_->childItems();
    for (QGraphicsItem* kid : kids) {
        drop_overlay_->removeFromGroup(kid);
        delete kid;
    }

    if (!has_content) {
        for (const PanelRow& r : rows)
            drop_overlay_->addToGroup(scene_.addRect(
                QRectF(kSceneMargin, r.top, content_w, r.h),
                QPen(t.border_soft, 1.0), QBrush(t.surface_low)));
        if (v_count > 0 && a_count > 0) {
            const auto v1 = std::find_if(rows.begin(), rows.end(),
                                         [](const PanelRow& r) { return r.flat == 0; });
            const auto a1 = std::find_if(rows.begin(), rows.end(),
                                         [v_count](const PanelRow& r) { return r.flat == v_count; });
            if (v1 != rows.end() && a1 != rows.end()) {
                const double v_bottom = v1->top + v1->h;
                drop_overlay_->addToGroup(
                    scene_.addRect(QRectF(kSceneMargin, v_bottom, content_w, a1->top - v_bottom),
                                   QPen(Qt::NoPen), QBrush(t.surface_highest)));
            }
        }
    }

    QColor lane_fill = t.accent;
    lane_fill.setAlpha(26);
    drop_overlay_->addToGroup(scene_.addRect(
        QRectF(kSceneMargin, lane_top, content_w, lane_h),
        QPen(t.accent, 1.5), QBrush(lane_fill)));

    QColor ghost_fill = t.accent;
    ghost_fill.setAlpha(42);
    drop_overlay_->addToGroup(scene_.addRect(QRectF(x, lane_top, w, lane_h),
                                             QPen(t.accent, 2.0), QBrush(ghost_fill)));

    if (meta.is_video && a_count > 0) {
        const auto mate = rows.empty() ? rows.end()
                                       : std::find_if(rows.begin(), rows.end(),
                                                      [v_count](const PanelRow& r) {
                                                          return r.flat == v_count;
                                                      });
        if (mate != rows.end())
            drop_overlay_->addToGroup(
                scene_.addRect(QRectF(x, mate->top, w, mate->h),
                               QPen(t.accent, 1.5), QBrush(ghost_fill)));
    }

    QColor guide_c = t.accent;
    guide_c.setAlpha(150);
    drop_overlay_->addToGroup(scene_.addLine(QLineF(x, stack_top, x, stack_bot),
                                             QPen(guide_c, 1.5)));

    if (w >= 64.0) {
        const QString lane_name =
            meta.is_video ? QStringLiteral("V%1").arg(lane.index + 1)
                          : QStringLiteral("A%1").arg(lane.index + 1);
        QString tag = lane_name + QStringLiteral(" · ")
                      + timecode(frame, fps_ > 0.0 ? fps_ : meta.fps);
        QFont tag_font;
        tag_font.setPointSizeF(8.0);
        tag_font.setBold(true);
        QFontMetricsF fm(tag_font);
        if (fm.horizontalAdvance(tag) > w - 20.0)
            tag = fm.elidedText(tag, Qt::ElideRight, static_cast<int>(w - 20.0));
        const double tw = fm.horizontalAdvance(tag);
        const double th = fm.height();
        const QRectF tag_rect(x + 4.0, lane_top + 4.0, tw + 12.0, th + 6.0);
        auto* pill = new QGraphicsPathItem(rounded_rect_path(tag_rect, 5.0));
        pill->setBrush(QColor(0, 0, 0, 175));
        pill->setPen(QPen(t.accent, 1.0));
        drop_overlay_->addToGroup(pill);
        auto* tag_text = new QGraphicsSimpleTextItem(tag);
        tag_text->setFont(tag_font);
        tag_text->setBrush(t.ink);
        tag_text->setPos(tag_rect.left() + 6.0, tag_rect.top() + 3.0);
        drop_overlay_->addToGroup(tag_text);
    }
}

void TimelineWidget::leaveEvent(QEvent* event) {
    if (hover_flat_ != -1) clear_row_highlight(hover_highlight_, hover_flat_);
    if (drop_lane_flat_ != -1) clear_row_highlight(drop_lane_highlight_, drop_lane_flat_);
    clear_drop_preview();
    QGraphicsView::leaveEvent(event);
}

void TimelineWidget::dragLeaveEvent(QDragLeaveEvent* event) {
    if (drop_lane_flat_ != -1) clear_row_highlight(drop_lane_highlight_, drop_lane_flat_);
    clear_drop_preview();
    event->accept();
}

int64_t TimelineWidget::snap_frame(int64_t frame) const {
    if (!snap_enabled_ || fps_ <= 0.0 || frames_per_pixel_ <= 0.0) return frame;
    const auto targets = collect_snap_targets({}, false);
    const int64_t max_delta = std::max<int64_t>(
        1, static_cast<int64_t>(
               std::llround(timeline_snap::kSnapRadiusPx * frames_per_pixel_)));
    const int64_t edge = timeline_snap::snap_frame_to_edges(frame, max_delta, targets);
    if (edge != frame) return edge;
    return timeline_snap::snap_to_grid(frame, frames_per_pixel_);
}

int64_t TimelineWidget::blade_cut_frame(const int x) const {
    const QPointF scene_p = mapToScene(QPoint(x, 0));
    const double scene_x = scene_p.x();
    return static_cast<int64_t>(
        std::llround((scene_x - kSceneMargin - kTrackHeaderWidth) * frames_per_pixel_));
}

std::vector<int64_t> TimelineWidget::collect_snap_targets(
    const std::vector<canvas::core::ClipId>& exclude,
    const bool include_playhead) const {
    std::vector<int64_t> t;
    const auto add_clip = [&](const canvas::core::Clip& c) {
        if (std::find(exclude.begin(), exclude.end(), c.id) != exclude.end()) return;
        t.push_back(c.tl_in);
        t.push_back(c.tl_out);
    };
    if (sequence_) {
        for (const auto& tr : sequence_->video_tracks)
            for (const auto& c : tr.clips) add_clip(c);
        for (const auto& tr : sequence_->audio_tracks)
            for (const auto& c : tr.clips) add_clip(c);
        for (const auto& b : sequence_->bookmarks) t.push_back(b.frame);
    }
    if (include_playhead) t.push_back(playhead_frame_);
    std::sort(t.begin(), t.end());
    t.erase(std::unique(t.begin(), t.end()), t.end());
    return t;
}

int64_t TimelineWidget::snap_trim_edge(const int64_t raw_edge, ClipItem* clip) {
    if (!snap_enabled_ || !clip || frames_per_pixel_ <= 0.0) return raw_edge;
    std::vector<canvas::core::ClipId> exclude;
    exclude.push_back(clip->clip->id);
    if (ClipItem* mate = find_linked_mate(clip)) exclude.push_back(mate->clip->id);
    const std::vector<int64_t> targets =
        collect_snap_targets(exclude, true);
    const int64_t max_delta = std::max<int64_t>(
        1, static_cast<int64_t>(
               std::llround(timeline_snap::kSnapRadiusPx * frames_per_pixel_)));
    return timeline_snap::snap_frame_to_edges(raw_edge, max_delta, targets);
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

TimelineWidget::ClipItem* TimelineWidget::find_clip_item(canvas::core::ClipId id) {
    for (auto& item : clip_items_)
        if (item.clip && item.clip->id == id) return &item;
    return nullptr;
}

bool TimelineWidget::find_volume_line_hit(const QPointF& scene_pos, ClipItem*& out_clip) {
    for (auto& item : clip_items_) {
        if (item.track_kind != canvas::core::Track::Kind::Audio || !item.volume_line)
            continue;
        if (!item.rect || !item.rect->sceneBoundingRect().contains(scene_pos)) continue;
        const QLineF l = item.volume_line->line();
        const QPointF c = item.volume_line->mapToScene(l.center());
        if (scene_pos.x() >= l.x1() && scene_pos.x() <= l.x2() &&
            std::abs(scene_pos.y() - c.y()) <= timeline_volume_line::kVolumeLineHitBandPx) {
            out_clip = &item;
            return true;
        }
    }
    return false;
}

void TimelineWidget::position_clip_at(ClipItem& item, int64_t tl_in) {
    if (!item.rect) return;
    const int v_count = sequence_ ? static_cast<int>(sequence_->video_tracks.size()) : 1;
    const double left = kSceneMargin + kTrackHeaderWidth + tl_in / frames_per_pixel_;
    const double top = static_cast<double>(track_top(item.track_index, v_count));
    const double cy = top + (item.track_kind == canvas::core::Track::Kind::Video ? 2 : 3);

    QGraphicsItem* group = item.rect->parentItem();
    if (!group) return;
    const QPointF new_base(left, cy);
    const QPointF delta = new_base - item.rect->scenePos();
    group->moveBy(delta.x(), delta.y());
}

void TimelineWidget::preview_trim_clip(ClipItem& item, const TrimEdge edge, const int64_t tl_frame) {
    if (!item.rect || !item.clip) return;
    QGraphicsItem* group = item.rect->parentItem();
    if (!group) return;

    int64_t edge_frame = tl_frame;
    if (edge == TrimEdge::Head) {
        edge_frame = std::max(edge_frame, item.clip->tl_in - item.clip->src_in);
        edge_frame = std::min(edge_frame, item.clip->tl_out - 1);
    } else {
        const auto mit = media_paths_.find(item.clip->media);
        if (mit != media_paths_.end() && mit->second.total_frames > 0)
            edge_frame =
                std::min(edge_frame, item.clip->tl_out +
                                         (mit->second.total_frames - item.clip->src_out));
        edge_frame = std::max(edge_frame, item.clip->tl_in + 1);
    }

    const double left = kSceneMargin + kTrackHeaderWidth;
    const int64_t new_tl_in = edge == TrimEdge::Head ? edge_frame : item.clip->tl_in;
    const int64_t right = edge == TrimEdge::Head ? item.clip->tl_out : edge_frame;
    const double cx = left + new_tl_in / frames_per_pixel_;
    const double cw = std::max(0.0, (right - new_tl_in) / frames_per_pixel_);

    const double cy = item.rect->sceneBoundingRect().top();
    const double ch = item.rect->rect().height();
    const double label_h = std::min(kClipLabelHeight, ch * 0.35);
    const double body_h = ch - label_h;

    if (auto* cg = dynamic_cast<ClipClipGroup*>(group))
        cg->set_shape(QRectF(cx, cy, cw, ch));
    item.rect->setPos(cx, cy);
    item.rect->setRect(QRectF(0, 0, cw, ch));
    if (item.shell)
        item.shell->setPos(cx, cy),
            item.shell->setPath(rounded_rect_path(QRectF(0, 0, cw, ch), 6));
    if (item.outline)
        item.outline->setPos(cx, cy),
            item.outline->setPath(rounded_rect_path(
                QRectF(kClipOutlineW / 2.0, kClipOutlineW / 2.0,
                       std::max(0.0, cw - kClipOutlineW), ch - kClipOutlineW), 6));
    if (item.label_bar)
        item.label_bar->setPos(cx, cy),
            item.label_bar->setPath(rounded_rect_path(QRectF(0, body_h, cw, label_h), 6));
    if (item.text) item.text->setPos(cx + 17, cy + body_h + 1);

    const int num = std::max(1, static_cast<int>(item.cells.size()));
    const double cell_w = cw / num;
    for (std::size_t c = 0; c < item.cells.size(); ++c)
        if (item.cells[c].item) item.cells[c].item->setPos(cx + c * cell_w, cy + 2);
}

bool TimelineWidget::reacquire_dragged_clip(const canvas::core::ClipId id,
                                            const canvas::core::ClipId mate_id,
                                            const int64_t pointer_frame) {
    if (!is_dragging_ || !dragged_clip_) return false;
    for (auto& it : clip_items_) {
        if (!it.clip || it.clip->id != id) continue;
        dragged_clip_ = &it;
        original_track_index_ = it.track_index;
        snap_targets_ = collect_snap_targets(selection_.ids(), true);
        drag_ctrl_.begin(pointer_frame, it.clip->tl_in, it.track_index,
                         it.clip->tl_out - it.clip->tl_in);
        drag_mate_ = find_linked_mate(&it);
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
    const auto st_t0 = std::chrono::steady_clock::now();
    if (debug_enabled() && (scrub_log_++ % 30) == 0)
        qDebug() << "timeline: scrub_to_frame" << frame;
    if ((scrub_log_++ & 3u) == 0u) {
        static int64_t last_log = -1;
        static auto last_t = QDateTime::currentDateTime();
        static bool first = true;
        auto now = QDateTime::currentDateTime();
        double dt_ms = 0.0;
        if (!first) dt_ms = last_t.msecsTo(now);
        qDebug() << "[scrub] TIMELINE scrub_to_frame=" << frame
                   << "dt_ms=" << dt_ms
                   << "delta=" << (last_log >= 0 ? (frame - last_log) : 0)
                   << "px_per_move=" << (last_log >= 0 ? std::abs(frame - last_log) : 0);
        last_log = frame;
        last_t = now;
        first = false;
    }
    set_playhead_position(snap_frame(frame));
    const double cost_ms = std::chrono::duration<double, std::milli>(
                               std::chrono::steady_clock::now() - st_t0).count();
    static auto s_agg_at = std::chrono::steady_clock::now();
    static int s_agg_n = 0;
    static double s_agg_ms = 0.0, s_max_ms = 0.0;
    ++s_agg_n;
    s_agg_ms += cost_ms;
    s_max_ms = std::max(s_max_ms, cost_ms);
    const auto s_now = std::chrono::steady_clock::now();
    if (s_agg_n == 1 || s_now - s_agg_at >= std::chrono::seconds(1)) {
        s_agg_at = s_now;
        qDebug() << "[scrub] UI cost_ms=" << cost_ms
                   << "avg_ms=" << QString::number(s_agg_ms / s_agg_n, 'f', 2)
                   << "max_ms=" << QString::number(s_max_ms, 'f', 2)
                   << "n=" << s_agg_n;
        s_agg_n = 0;
        s_agg_ms = 0.0;
        s_max_ms = 0.0;
    }
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
                QPen(tokens().accent, 1, Qt::DashLine));
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
}

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
    for (const auto& a : track.clips) {
        for (const auto& b : track.clips) {
            if (&a == &b) continue;
            if (b.tl_in != a.tl_out) continue;
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
            consider(c.tl_in, Edge::Start, &c);
            bool has_right_neighbour = false;
            for (const auto& o : track.clips)
                if (o.tl_in == c.tl_out) { has_right_neighbour = true; break; }
            if (!has_right_neighbour) consider(c.tl_out, Edge::End, &c);
        }
    }
    return t;
}

bool TimelineWidget::find_trim_edge(const QPointF& p, ClipItem*& out_clip,
                                    TrimEdge& out_edge) {
    out_clip = nullptr;
    out_edge = TrimEdge::Head;
    if (!sequence_ || p.x() < kSceneMargin + kTrackHeaderWidth) return false;
    const int v_count = static_cast<int>(sequence_->video_tracks.size());
    const int flat = track_at_y(p.y(), v_count);
    if (flat < 0) return false;

    constexpr double kTrimGrabPx = 6.0;
    const double left = kSceneMargin + kTrackHeaderWidth;
    for (auto& item : clip_items_) {
        if (!item.clip || item.track_index != flat) continue;
        const double cleft = left + item.clip->tl_in / frames_per_pixel_;
        const double cright = left + item.clip->tl_out / frames_per_pixel_;
        const bool near_head = std::abs(p.x() - cleft) <= kTrimGrabPx;
        const bool near_tail = std::abs(p.x() - cright) <= kTrimGrabPx;
        if (!near_head && !near_tail) continue;
        if (near_head == near_tail) continue;
        const QRectF body = item.rect ? item.rect->sceneBoundingRect() : QRectF();
        if (body.isValid() && p.y() > body.bottom() - kClipLabelHeight) continue;
        out_clip = &item;
        out_edge = near_head ? TrimEdge::Head : TrimEdge::Tail;
        return true;
    }
    return false;
}

void TimelineWidget::update_transition_hover(const QPointF& p) {
    if (transition_editor_.dragging()) return;

    const CutTarget t = cut_at_scene_pos(p);
    if (!t.valid()) {
        hide_transition_handle();
        return;
    }

    const int64_t seeded = t.edge == Edge::Start ? t.a->transition_in_duration
                                                 : t.a->transition_out_duration;
    if (!transition_editor_.open(t, seeded)) return;
    if (transition_editor_.has_transition()) {
        hide_transition_handle();
        return;
    }
    rebuild_transition_handle();

    if (transition_editor_.visible() && p.x() <= kSceneMargin + kTrackHeaderWidth) {
        hide_transition_handle();
    }
}

void TimelineWidget::hide_transition_handle() {
    if (transition_editor_.visible()) unsetCursor();
    transition_editor_.close();
    transition_press_armed_ = false;
    for (QGraphicsItem* item : transition_items_) {
        if (item->scene() == &scene_) scene_.removeItem(item);
        delete item;
    }
    transition_items_.clear();
    transition_overlay_ = nullptr;
    transition_icon_ = nullptr;
}

void TimelineWidget::rebuild_transition_handle() {
    const CutTarget& target = transition_editor_.target();
    const int64_t dur = transition_editor_.duration();
    const int64_t left_frame = transition_editor_.left();
    const int64_t right_frame = transition_editor_.right();
    if (!target.valid() || !transition_editor_.visible()) return;

    for (QGraphicsItem* item : transition_items_) {
        if (item->scene() == &scene_) scene_.removeItem(item);
        delete item;
    }
    transition_items_.clear();
    transition_overlay_ = nullptr;
    transition_icon_ = nullptr;

    const int v_count = static_cast<int>(sequence_ ? sequence_->video_tracks.size() : 1);
    const double track_top_y =
        static_cast<double>(track_top(target.track_index, v_count));
    const double row_y = track_top_y + 3;
    const double row_h = track_height(target.track_index, v_count) - 8.0;

    auto scene_x = [this](int64_t frame) {
        return kSceneMargin + kTrackHeaderWidth + frame / frames_per_pixel_;
    };
    const double cut_x = scene_x(target.cut_frame);
    const bool is_edge = !target.is_cut();
    double left_x, right_x;
    if (is_edge && target.edge == Edge::Start) {
        left_x = cut_x;
        right_x = cut_x + dur / frames_per_pixel_;
    } else if (is_edge && target.edge == Edge::End) {
        left_x = cut_x - dur / frames_per_pixel_;
        right_x = cut_x;
    } else {
        left_x = scene_x(left_frame);
        right_x = scene_x(right_frame);
    }
    const double overlay_w = std::max(2.0, right_x - left_x);

    const QRectF ovr(left_x, row_y, overlay_w, row_h);
    QColor overlay_fill = tokens().accent;
    overlay_fill.setAlpha(60);
    transition_overlay_ =
        scene_.addPath(canvas_rounded_rect_path(ovr, 7.0), QPen(Qt::NoPen),
                       QBrush(overlay_fill));
    auto* frost = new QGraphicsBlurEffect;
    frost->setBlurRadius(1.2);
    transition_overlay_->setGraphicsEffect(frost);
    transition_overlay_->setZValue(60);
    transition_items_.push_back(transition_overlay_);

    const double icx = cut_x;
    const double icy = row_y + row_h / 2.0;
    const QString icon_name =
        target.edge == Edge::Start   ? QStringLiteral("transition_in")
        : target.edge == Edge::End   ? QStringLiteral("transition_out")
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
    QColor glyph_tint = tokens().icon;
    glyph_tint.setAlpha(235);
    tinted.fill(glyph_tint);
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
}

void TimelineWidget::press_transition_handle(const QPointF& p) {
    if (!transition_editor_.visible() || !transition_editor_.target().valid()) return;
    if (!transition_editor_.has_transition()) return;
    const int64_t left = transition_editor_.left();
    const int64_t right = transition_editor_.right();
    const double lx = kSceneMargin + kTrackHeaderWidth + left / frames_per_pixel_;
    const double rx = kSceneMargin + kTrackHeaderWidth + right / frames_per_pixel_;
    constexpr double kEdgeGrab = 7.0;

    int edge = kTransitionEdgeNone;
    if (std::abs(p.x() - lx) <= kEdgeGrab) edge = kTransitionEdgeLeft;
    else if (std::abs(p.x() - rx) <= kEdgeGrab) edge = kTransitionEdgeRight;
    if (edge < 0) return;

    transition_editor_.begin_drag(edge, false);
    setCursor(Qt::SizeHorCursor);
}

void TimelineWidget::move_transition_handle(const QPointF& p) {
    if (!transition_editor_.dragging()) return;
    const int64_t edge_frame = static_cast<int64_t>(
        (p.x() - kSceneMargin - kTrackHeaderWidth) * frames_per_pixel_);
    transition_editor_.move_to(edge_frame);
    rebuild_transition_handle();

    if (transition_editor_.snap() && transition_editor_.target().valid()) {
        const bool in_edge = transition_editor_.target().edge == Edge::Start;
        for (auto& b : transition_bubbles_) {
            if (b.pill && b.in_edge == in_edge &&
                b.clip_id == transition_editor_.target().a->id)
                refresh_transition_bubble(b, transition_editor_.duration());
        }
    }
}

void TimelineWidget::release_transition_handle() {
    if (!transition_editor_.dragging()) return;
    transition_editor_.end_drag();
    unsetCursor();
    const CutTarget& target = transition_editor_.target();
    if (target.valid() &&
        transition_editor_.duration() != transition_editor_.stored_duration()) {
        if (target.edge == Edge::Start)
            emit transition_in_resized(target.a, transition_editor_.duration());
        else
            emit transition_resized(target.a, transition_editor_.duration());
    }
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

    const CutTarget target{clip, nullptr, kind, flat, frame,
                           in_edge ? Edge::Start : Edge::End};
    const int64_t seeded = in_edge ? clip->transition_in_duration
                                   : clip->transition_out_duration;
    if (transition_editor_.open(target, seeded)) rebuild_transition_handle();
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

    const double lx = kSceneMargin + kTrackHeaderWidth +
                      transition_editor_.left() / frames_per_pixel_;
    const double rx = kSceneMargin + kTrackHeaderWidth +
                      transition_editor_.right() / frames_per_pixel_;
    const double dl = std::abs(scene_pos.x() - lx);
    const double dr = std::abs(scene_pos.x() - rx);
    const int edge = dr < dl ? kTransitionEdgeRight : kTransitionEdgeLeft;
    transition_editor_.begin_drag(edge, true);
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
    for (auto& bb : transition_bubbles_) {
        if (!bb.pill) continue;
        const bool sel = bb.clip_id == selected_transition_.a &&
                         (bb.cut
                              ? bb.b_clip_id == selected_transition_.b
                              : bb.in_edge == selected_transition_.in_edge);
        bb.pill->setPen(sel ? QPen(QColor(0xFF, 0xD7, 0x4A, 255), 2.0)
                            : QPen(tokens().ink, 1.0));
        bb.pill->setZValue(sel ? 52 : 50);
    }
    emit transition_selected(selected_transition_.a, selected_transition_.b,
                             selected_transition_.in_edge);
}

void TimelineWidget::clear_selected_transition() {
    selected_transition_ = {};
    for (auto& bb : transition_bubbles_) {
        if (bb.pill) bb.pill->setPen(QPen(tokens().ink, 1.0));
    }
    emit transition_selection_cleared();
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

    const CutTarget target{a, b, kind, flat, cut_frame, Edge::Cut};
    const int64_t seeded =
        a->transition_out_duration >= kMinTransitionFrames
            ? a->transition_out_duration
            : (b->transition_in_duration >= kMinTransitionFrames
                   ? b->transition_in_duration
                   : 6);
    if (transition_editor_.open(target, seeded)) rebuild_transition_handle();
}

void TimelineWidget::apply_selection_highlight() {
    int highlighted = 0;
    for (auto& item : clip_items_) {
        if (!item.outline) continue;
        const bool sel = item.clip && selection_.contains(item.clip->id);
        const double w = sel ? kClipSelectedOutlineW : kClipOutlineW;
        const QColor color = sel
            ? (item.track_kind == canvas::core::Track::Kind::Audio
                   ? tokens().accent_text
                   : QColor(0x3B, 0x82, 0xF6))
            : (item.track_kind == canvas::core::Track::Kind::Audio
                   ? tokens().clip_border_audio
                   : tokens().clip_border_video);
        item.outline->setPen(QPen(color, w));
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

    const double ruler_top = kMinimapHeight + kSceneMargin + kTimecodeBarHeight;
    const int vy = event->pos().y();
    const bool in_ruler = vy < ruler_top + kRulerHeight;

    const double playhead_x = kSceneMargin + kTrackHeaderWidth + playhead_frame_ / frames_per_pixel_;
    constexpr double kPlayheadGrabPx = 5.0;
    const bool on_playhead =
        vy > kTimecodeBarHeight + kSceneMargin + kMinimapHeight &&
        std::abs(scene_pos.x() - playhead_x) <= kPlayheadGrabPx;

    const bool left = event->button() == Qt::LeftButton;
    const bool middle = event->button() == Qt::MiddleButton;
    if ((left || middle) && in_ruler) {
        set_follow_playhead(true);
        const int64_t snapped = snap_frame(frame_at_x(event->pos().x()));
        emit playhead_moved(snapped);
        set_playhead_position(snapped);
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
            set_follow_playhead(true);
            const int64_t snapped = snap_frame(frame);
            set_playhead_position(snapped);
            emit playhead_moved(snapped);
        }
        event->accept();
        return;
    }

    if (event->button() != Qt::LeftButton) {
        QGraphicsView::mousePressEvent(event);
        return;
    }

    if (sequence_ && has_timeline_content()) {
        const int v_count = static_cast<int>(sequence_->video_tracks.size());
        const int a_count = static_cast<int>(sequence_->audio_tracks.size());
        const bool in_header_strip = scene_pos.x() < kSceneMargin + kTrackHeaderWidth;
        const int edge = header_resize_target(scene_pos.y(), v_count, a_count);
        const bool on_band_edge = edge == v_count || edge == v_count + 1;
        const bool grab_pan = in_section_divider_band(scene_pos.y(), v_count, a_count) && edge < 0;
        const bool grab_row = !grab_pan && edge >= 0 && (in_header_strip || on_band_edge);
        if (debug_enabled())
            qDebug() << "timeline: header press x=" << scene_pos.x() << "y=" << scene_pos.y()
                     << "in_header_strip=" << in_header_strip << "edge=" << edge
                     << "v=" << v_count << "a=" << a_count
                     << "grab_pan=" << grab_pan << "grab_row=" << grab_row;
        if (grab_pan) {
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

    if (sequence_ && scene_pos.x() < kSceneMargin + kTrackHeaderWidth) {
        const auto hit_icons = [&](const std::vector<TrackHeader>& headers,
                                   const std::vector<canvas::core::Track>& tracks,
                                   canvas::core::Track::Kind kind) -> bool {
            const std::size_t n = std::min(headers.size(), tracks.size());
            for (std::size_t i = 0; i < n; ++i) {
                const auto& h = headers[i];
                if (h.mute_icon && h.mute_icon->sceneBoundingRect().contains(scene_pos)) {
                    emit track_mute_toggled(kind, static_cast<int>(i), !tracks[i].muted);
                    return true;
                }
                if (h.solo_icon && h.solo_icon->sceneBoundingRect().contains(scene_pos)) {
                    emit track_solo_toggled(kind, static_cast<int>(i), !tracks[i].solo);
                    return true;
                }
                if (h.lock_icon && h.lock_icon->sceneBoundingRect().contains(scene_pos)) {
                    emit track_lock_toggled(kind, static_cast<int>(i), !tracks[i].locked);
                    return true;
                }
                if (h.collapse_icon && h.collapse_icon->sceneBoundingRect().contains(scene_pos)) {
                    emit track_collapse_toggled(kind, static_cast<int>(i), !tracks[i].collapsed);
                    return true;
                }
            }
            return false;
        };
        if (hit_icons(audio_track_headers_, sequence_->audio_tracks,
                      canvas::core::Track::Kind::Audio) ||
            hit_icons(video_track_headers_, sequence_->video_tracks,
                      canvas::core::Track::Kind::Video)) {
            event->accept();
            return;
        }
    }

    if (current_tool_ != Tool::Blade) {
        ClipItem* trim_clip = nullptr;
        TrimEdge trim_edge = TrimEdge::Head;
        if (find_trim_edge(scene_pos, trim_clip, trim_edge)) {
            trimming_ = true;
            trimmed_clip_ = trim_clip;
            trim_edge_ = trim_edge;
            trim_start_edge_ =
                trim_edge_ == TrimEdge::Head ? trim_clip->clip->tl_in : trim_clip->clip->tl_out;
            trim_grab_offset_px_ =
                scene_pos.x() -
                (kSceneMargin + kTrackHeaderWidth +
                 trim_start_edge_ / frames_per_pixel_);
            set_selection({trim_clip->clip->id});
            emit clip_selected(trim_clip->clip);
            clear_selected_transition();
            hide_transition_handle();
            if (current_tool_ == Tool::Trim) setCursor(Qt::SizeHorCursor);
            event->accept();
            return;
        }
    }

    if (maybe_press_transition_bubble(scene_pos)) {
        event->accept();
        return;
    }
    press_transition_handle(scene_pos);
    if (transition_editor_.dragging()) {
        event->accept();
        return;
    }

    if (current_tool_ == Tool::Select) {
        ClipItem* vol_clip = nullptr;
        if (find_volume_line_hit(scene_pos, vol_clip)) {
            const auto mates =
                sequence_ ? timeline_selection::expand_with_mates({vol_clip->clip->id}, *sequence_)
                          : std::vector<canvas::core::ClipId>{vol_clip->clip->id};
            if (event->modifiers() & Qt::ControlModifier) {
                const bool already = selection_.contains(vol_clip->clip->id);
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
                auto acc = selection_.ids();
                for (const auto id : mates)
                    if (std::find(acc.begin(), acc.end(), id) == acc.end())
                        acc.push_back(id);
                set_selection(acc);
            } else if (!(selection_.size() > 1 && selection_.contains(vol_clip->clip->id))) {
                set_selection({vol_clip->clip->id});
            }
            emit clip_selected(vol_clip->clip);
            volume_drag_armed_ = true;
            volume_dragging_ = false;
            volume_drag_clip_ = vol_clip->clip->id;
            volume_drag_db_ = static_cast<float>(vol_clip->clip->volume_db);
            volume_drag_targets_.clear();
            if (sequence_) {
                const auto targets = resolve_audio_targets(*sequence_, selection_.ids());
                for (const auto& t : targets) volume_drag_targets_.push_back(t.id);
            }
            if (volume_drag_targets_.empty()) volume_drag_targets_.push_back(volume_drag_clip_);
            drag_press_pos_ = event->pos();
            setCursor(Qt::SizeVerCursor);
            event->accept();
            return;
        }
    }

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

    if (current_tool_ == Tool::Blade) {
        if (hit) {
            const int64_t cut = blade_cut_frame(event->pos().x());
            const char* kind =
                hit->track_kind == canvas::core::Track::Kind::Video ? "video" : "audio";
            qDebug().nospace()
                << "[blade] " << kind << " clip=" << hit->clip->id
                << " tl=[" << hit->clip->tl_in << "," << hit->clip->tl_out << ")"
                << " src=[" << hit->clip->src_in << "," << hit->clip->src_out << ")"
                << " mouse_x=" << event->pos().x()
                << " scene_x=" << mapToScene(event->pos()).x()
                << " click_frame=" << frame_at_x(event->pos().x())
                << " cut=" << cut;
            {
                const auto mit = media_paths_.find(hit->clip->media);
                if (mit != media_paths_.end() && mit->second.fps > 0.0) {
                    const int64_t tl_span = hit->clip->tl_out - hit->clip->tl_in;
                    const int64_t src_frame =
                        tl_span > 0
                            ? hit->clip->src_in +
                                  static_cast<int64_t>(std::llround(
                                      static_cast<double>(std::max<int64_t>(0,
                                                                           cut - hit->clip->tl_in)) *
                                      static_cast<double>(hit->clip->src_out -
                                                          hit->clip->src_in) /
                                      static_cast<double>(tl_span)))
                            : hit->clip->src_in;
                    const double audio_t = static_cast<double>(src_frame) / mit->second.fps;
                    qDebug().nospace()
                        << "[blade] A/V clip=" << hit->clip->id
                        << " media_fps=" << QString::number(mit->second.fps, 'g', 4)
                        << " media_total=" << mit->second.total_frames
                        << " cut_src_frame=" << src_frame
                        << " cut_audio_time_s=" << QString::number(audio_t, 'f', 3)
                        << " cut_lo=" << QString::number(
                               static_cast<double>(hit->clip->src_in) /
                                   static_cast<double>(mit->second.total_frames),
                               'g', 6);
                } else {
                    qDebug().nospace()
                        << "[blade] A/V clip=" << hit->clip->id
                        << " media_unresolved (media_id=" << hit->clip->media << ")";
                }
            }
            emit blade_requested(hit->clip, cut);
            event->accept();
            return;
        }
        const int v_count =
            sequence_ ? static_cast<int>(sequence_->video_tracks.size()) : 0;
        const int flat = track_at_y(scene_pos.y(), v_count);
        const char* zone = "body";
        if (in_ruler) zone = "ruler";
        else if (on_minimap) zone = "minimap";
        else if (flat < 0) zone = "pinned/band";
        std::string row_cover;
        if (sequence_ && flat >= 0 &&
            static_cast<std::size_t>(flat) <
                sequence_->video_tracks.size() + sequence_->audio_tracks.size()) {
            const int64_t f = frame_at_x(event->pos().x());
            const bool is_v = static_cast<std::size_t>(flat) < sequence_->video_tracks.size();
            const auto& tr = is_v
                                 ? sequence_->video_tracks[static_cast<std::size_t>(flat)]
                                 : sequence_->audio_tracks[static_cast<std::size_t>(flat - v_count)];
            for (const auto& c : tr.clips) {
                if (c.tl_out <= f + 1 && c.tl_in >= f - 1) continue;
                if (!row_cover.empty()) row_cover += ";";
                row_cover += "id=" + std::to_string(c.id) + "[" +
                             std::to_string(c.tl_in) + "," + std::to_string(c.tl_out) + ")";
                if (f >= c.tl_in && f < c.tl_out) row_cover += "*";
            }
            if (row_cover.empty()) row_cover = "none";
        } else {
            row_cover = "n/a";
        }
        qDebug().nospace()
            << "[blade] NO-HIT zone=" << zone
            << " row=" << flat
            << " scene_x=" << scene_pos.x()
            << " scene_y=" << scene_pos.y()
            << " mouse_x=" << event->pos().x()
            << " frame=" << frame_at_x(event->pos().x())
            << " row_cover=" << QString::fromStdString(row_cover);
        event->accept();
        return;
    }

    if (scene_pos.x() < kSceneMargin + kTrackHeaderWidth) {
        if (!in_ruler && !on_playhead) {
            is_selecting_range_ = true;
            marquee_full_height_ = true;
            drag_scene_start_ = scene_pos;
            drag_start_frame_ = 0;
            drag_start_pos_ = event->pos();
            if (!selection_rect_) {
                QColor sel_fill = tokens().accent;
                sel_fill.setAlpha(40);
                selection_rect_ = scene_.addRect(QRectF(), QPen(tokens().accent),
                                                 QBrush(sel_fill));
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
        if (in_ruler || on_playhead) {
            set_follow_playhead(true);
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
            QColor sel_fill = tokens().accent;
            sel_fill.setAlpha(40);
            selection_rect_ = scene_.addRect(QRectF(), QPen(tokens().accent),
                                             QBrush(sel_fill));
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
        auto acc = selection_.ids();
        for (const auto id : mates)
            if (std::find(acc.begin(), acc.end(), id) == acc.end())
                acc.push_back(id);
        set_selection(acc);
    } else {
        if (!(selection_.size() > 1 && selection_.contains(hit->clip->id)))
            set_selection({hit->clip->id});
    }
    emit clip_selected(hit->clip);
    if (debug_enabled()) {
        QString sel;
        for (const auto id : selection_.ids())
            sel += QString::number(static_cast<quint64>(id)) + QLatin1Char(' ');
        qDebug() << "timeline: select: SUCCESS after 1 click. clip id="
                 << static_cast<long long>(hit->clip->id)
                 << "selected set ->" << sel;
    }
    const int svc = sequence_ ? static_cast<int>(sequence_->video_tracks.size()) : 1;
    drag_clip_snapshot_.clear();
    drag_clip_orig_.clear();
    drag_primary_id_ = hit->clip->id;
    for (const auto id : selection_.ids()) {
        if (ClipItem* citem = find_clip_item(id)) {
            drag_clip_snapshot_.push_back(
                MovedClip{id, citem->clip->tl_in, citem->track_kind,
                          kind_track_index(citem->track_index, svc)});
            drag_clip_orig_.push_back(citem->clip->tl_in);
        }
    }

    dragged_clip_ = hit;
    original_track_index_ = hit->track_index;
    drag_mate_ = find_linked_mate(hit);
    drag_press_pos_ = event->pos();
    drag_start_frame_ = frame_at_x(event->pos().x());
    drag_grab_offset_px_ = scene_pos.x() - hit->rect->rect().left();
    {
        std::vector<canvas::core::ClipId> exclude = selection_.ids();
        exclude.push_back(hit->clip->id);
        if (drag_mate_ && drag_mate_->clip) exclude.push_back(drag_mate_->clip->id);
        snap_targets_ = collect_snap_targets(exclude, true);
    }
    drag_ctrl_.begin(frame_at_x(event->pos().x()), hit->clip->tl_in, hit->track_index,
                     hit->clip->tl_out - hit->clip->tl_in);
    is_dragging_ = false;
    event->accept();
}

void TimelineWidget::mouseMoveEvent(QMouseEvent* event) {
    if (pan_dragging_) {
        const double dy = event->pos().y() - pan_anchor_viewport_y_;
        if (auto* bar = verticalScrollBar()) {
            const int before = bar->value();
            const int target = static_cast<int>(lround(pan_start_scroll_ - dy));
            bar->setValue(std::clamp(target, 0, bar->maximum()));
            if (debug_enabled())
                qDebug() << "timeline: pan move dy=" << dy << "scroll" << before << "->"
                         << bar->value() << "sceneTopAtViewport=" << mapToScene(0, 0).y();
        }
        event->accept();
        return;
    }

    if (resizing_track_) {
        const auto tr_t0 = std::chrono::steady_clock::now();
        const int v_count = sequence_ ? static_cast<int>(sequence_->video_tracks.size()) : 0;
        const QPointF sp = mapToScene(event->pos());
        const double dy = sp.y() - resize_start_y_;
        if (resize_edge_ == 0) {
            track_v_pad_top_ =
                std::clamp(resize_below_start_ + dy, kMinTrackVPad, kMaxTrackVPad);
        } else if (resize_edge_ >= resize_total_) {
            track_v_pad_bottom_ =
                std::clamp(resize_above_start_ + dy, kMinTrackVPad, kMaxTrackVPad);
        } else {
            const int up_flat = flat_of_screen_row(resize_edge_ - 1, v_count);
            const int dn_flat = flat_of_screen_row(resize_edge_, v_count);
            const auto flat_collapsed = [&](int flat) {
                if (!sequence_ || flat < 0) return false;
                if (flat < v_count)
                    return static_cast<std::size_t>(flat) < sequence_->video_tracks.size() &&
                           sequence_->video_tracks[static_cast<std::size_t>(flat)].collapsed;
                const int ai = flat - v_count;
                return static_cast<std::size_t>(ai) < sequence_->audio_tracks.size() &&
                       sequence_->audio_tracks[static_cast<std::size_t>(ai)].collapsed;
            };
            if (up_flat >= 0 && !flat_collapsed(up_flat))
                set_track_height(up_flat, v_count, resize_above_start_ + dy);
            if (dn_flat >= 0 && !flat_collapsed(dn_flat))
                set_track_height(dn_flat, v_count, resize_below_start_ - dy);
        }
        track_resize_timer_->start();
        const double tr_ms = std::chrono::duration<double, std::milli>(
                                 std::chrono::steady_clock::now() - tr_t0).count();
        static auto s_tr_at = std::chrono::steady_clock::now();
        static int s_tr_n = 0;
        static double s_tr_ms = 0.0, s_max_ms = 0.0;
        ++s_tr_n;
        s_tr_ms += tr_ms;
        s_max_ms = std::max(s_max_ms, tr_ms);
        const auto tr_now = std::chrono::steady_clock::now();
        if (s_tr_n == 1 || tr_now - s_tr_at >= std::chrono::seconds(1)) {
            s_tr_at = tr_now;
            qDebug() << "[ui:timeline] track_resize ms_avg=" << QString::number(s_tr_ms / s_tr_n, 'f', 2)
                       << "ms_last=" << QString::number(tr_ms, 'f', 2)
                       << "ms_max=" << QString::number(s_max_ms, 'f', 2)
                       << "moves/s=" << s_tr_n
                       << "edge=" << resize_edge_ << "/" << resize_total_;
            s_tr_n = 0;
            s_tr_ms = 0.0;
            s_max_ms = 0.0;
        }
        event->accept();
        return;
    }

    if (is_scrubbing_) {
        scrub_to_frame(frame_at_x(event->pos().x()));
        event->accept();
        return;
    }

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

    if (transition_editor_.dragging()) {
        move_transition_handle(mapToScene(event->pos()));
        event->accept();
        return;
    }

    if (volume_drag_armed_) {
        if (!volume_dragging_ &&
            (event->pos() - drag_press_pos_).manhattanLength() >= QApplication::startDragDistance()) {
            volume_dragging_ = true;
            setCursor(Qt::SizeVerCursor);
        }
        if (volume_dragging_) {
            const QPointF sp = mapToScene(event->pos());
            ClipItem* item = find_clip_item(volume_drag_clip_);
            if (item && item->volume_line) {
                const double rel_y = sp.y() - item->volume_y0;
                const double db = timeline_volume_line::drag_db_from_relative_y(
                    rel_y, item->volume_h, timeline_volume_line::kVolumeLineDragExponent);
                volume_drag_db_ = static_cast<float>(db);
                if (volume_drag_targets_.empty()) volume_drag_targets_.push_back(volume_drag_clip_);
                for (const auto id : volume_drag_targets_)
                    set_live_clip_gain(id, volume_drag_db_);
                emit volume_line_preview(volume_drag_db_);
                QToolTip::showText(QCursor::pos(), QString::asprintf("%+.1f dB", volume_drag_db_),
                                   this);
            }
        }
        event->accept();
        return;
    }

    if (!is_selecting_range_ && !is_dragging_ && !dragged_clip_ &&
        !transition_press_armed_ && !resizing_track_) {
        const QPointF sp = mapToScene(event->pos());
        bool hovering_edge = false;
        if (sequence_) {
            const int v_count = static_cast<int>(sequence_->video_tracks.size());
            const int a_count = static_cast<int>(sequence_->audio_tracks.size());
            const int edge = header_resize_target(sp.y(), v_count, a_count);
            const bool in_band =
                in_section_divider_band(sp.y(), v_count, a_count);
            if (sp.x() < kSceneMargin + kTrackHeaderWidth) {
                hovering_edge = edge >= 0 || in_band;
            } else if (v_count > 0 && a_count > 0) {
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
        if (!hovering_edge) {
            ClipItem* vc = nullptr;
            if (find_volume_line_hit(sp, vc)) {
                if (!track_resize_cursor_shown_) {
                    setCursor(Qt::SizeVerCursor);
                    track_resize_cursor_shown_ = true;
                }
            } else if (track_resize_cursor_shown_) {
                unsetCursor();
                track_resize_cursor_shown_ = false;
            }
        }
    }

    if (trimming_ && trimmed_clip_) {
        const double scene_x = mapToScene(event->pos()).x();
        const double edge_x = scene_x - trim_grab_offset_px_;
        int64_t new_edge = std::max<int64_t>(
            0, static_cast<int64_t>(std::floor(
                   (edge_x - kSceneMargin - kTrackHeaderWidth) * frames_per_pixel_)));
        new_edge = snap_trim_edge(new_edge, trimmed_clip_);
        const int64_t clamped =
            trim_edge_ == TrimEdge::Head
                ? std::min(new_edge, trimmed_clip_->clip->tl_out - 1)
                : std::max(new_edge, trimmed_clip_->clip->tl_in + 1);
        preview_trim_clip(*trimmed_clip_, trim_edge_, clamped);
        if (ClipItem* mate = find_linked_mate(trimmed_clip_))
            preview_trim_clip(*mate, trim_edge_, clamped);
        std::unordered_set<canvas::core::ClipId> trim_ids{trimmed_clip_->clip->id};
        if (ClipItem* mate = find_linked_mate(trimmed_clip_)) trim_ids.insert(mate->clip->id);
        shift_transition_bubbles(trim_ids);
        event->accept();
        return;
    }

    if (dragged_clip_ && !is_dragging_ && !is_selecting_range_) {
        if ((event->pos() - drag_press_pos_).manhattanLength() >= QApplication::startDragDistance()) {
            is_dragging_ = true;
        } else {
            event->accept();
            return;
        }
    }

    if (current_tool_ == Tool::Select && !is_dragging_ && !is_selecting_range_ &&
        !transition_press_armed_ && !transition_editor_.dragging()) {
        update_transition_hover(mapToScene(event->pos()));
    }

    if (current_tool_ == Tool::Blade && !is_dragging_ && !is_selecting_range_) {
        update_blade_preview(blade_cut_frame(event->pos().x()));
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

        if (dragged_clip_->track_kind == canvas::core::Track::Kind::Video && !promote_latched_ &&
            sequence_ && candidate < 0 &&
            scene_y < static_cast<double>(track_top(0, v_count))) {
            const int64_t snap_tl =
                drag_ctrl_.move(frame_at_x(event->pos().x()), candidate, v_count,
                                dragged_clip_->track_kind, snap_enabled_ && fps_ > 0.0,
                                frames_per_pixel_, snap_targets_)
                    .new_tl_in;
            promote_latched_ = true;
            const canvas::core::ClipId mate_id =
                drag_mate_ && drag_mate_->clip ? drag_mate_->clip->id : 0;
            emit new_upper_track_requested(dragged_clip_->clip->id, snap_tl);
            if (!reacquire_dragged_clip(dragged_clip_->clip->id, mate_id,
                                        frame_at_x(event->pos().x()))) {
                is_dragging_ = false;
                drag_ctrl_.end();
                snap_targets_.clear();
                dragged_clip_ = nullptr;
                drag_mate_ = nullptr;
                drag_clip_snapshot_.clear();
                drag_clip_orig_.clear();
                drag_primary_id_ = 0;
                event->accept();
                return;
            }
        }

        const auto drag_t0 = std::chrono::steady_clock::now();
        const auto res = drag_ctrl_.move(
            frame_at_x(event->pos().x()), candidate, v_count, dragged_clip_->track_kind,
            snap_enabled_ && fps_ > 0.0, frames_per_pixel_, snap_targets_);
        (void)drag_t0;

        const int target = drag_ctrl_.target_track_index();
        if (target >= 0) {
            dragged_clip_->track_kind =
                target < v_count ? canvas::core::Track::Kind::Video : canvas::core::Track::Kind::Audio;
            dragged_clip_->track_index = target;
        }

        int64_t primary_orig = 0;
        for (std::size_t si = 0; si < drag_clip_snapshot_.size(); ++si) {
            if (drag_clip_snapshot_[si].id == drag_primary_id_) {
                primary_orig = drag_clip_orig_[si];
                break;
            }
        }

        int64_t min_seq_orig = primary_orig;
        if (!drag_clip_orig_.empty())
            min_seq_orig = *std::min_element(drag_clip_orig_.begin(), drag_clip_orig_.end());
        const int64_t batch_delta =
            timeline_drag::clamp_batch_delta(primary_orig, res.new_tl_in, min_seq_orig);
        const int64_t primary_new = std::max<int64_t>(0, primary_orig + batch_delta);

        std::unordered_set<canvas::core::ClipId> dragged_ids{drag_primary_id_};
        if (drag_mate_ && drag_mate_->clip) dragged_ids.insert(drag_mate_->clip->id);
        for (const auto& s : drag_clip_snapshot_) dragged_ids.insert(s.id);
        if (drag_clip_snapshot_.size() >= 2 && !drag_clip_orig_.empty()) {
            for (std::size_t si = 0; si < drag_clip_snapshot_.size(); ++si) {
                auto& s = drag_clip_snapshot_[si];
                if (s.id == drag_primary_id_) continue;
                s.new_tl_in = timeline_drag::batch_target_tl_in(
                    primary_orig, res.new_tl_in, drag_clip_orig_[si], min_seq_orig);
                if (ClipItem* item = find_clip_item(s.id))
                    position_clip_at(*item, s.new_tl_in);
            }
        }
        if (drag_mate_) position_clip_at(*drag_mate_, primary_new);
        position_clip_at(*dragged_clip_, primary_new);
        shift_transition_bubbles(dragged_ids);
        update_snap_indicator(res.snapped, res.new_tl_in);

        static auto s_drag_at = std::chrono::steady_clock::now();
        static int s_drag_n = 0;
        static double s_drag_ms = 0.0, s_drag_max = 0.0;
        const double drag_ms = std::chrono::duration<double, std::milli>(
                                   std::chrono::steady_clock::now() - drag_t0).count();
        ++s_drag_n;
        s_drag_ms += drag_ms;
        s_drag_max = std::max(s_drag_max, drag_ms);
        const auto drag_now = std::chrono::steady_clock::now();
        if (s_drag_n == 1 || drag_now - s_drag_at >= std::chrono::seconds(1)) {
            s_drag_at = drag_now;
            qDebug() << "[ui:drag] move ms_avg=" << QString::number(s_drag_ms / s_drag_n, 'f', 2)
                       << "ms_last=" << QString::number(drag_ms, 'f', 2)
                       << "ms_max=" << QString::number(s_drag_max, 'f', 2)
                       << "moves/s=" << s_drag_n
                       << "snapped=" << (res.snapped ? 1 : 0);
            s_drag_n = 0;
            s_drag_ms = 0.0;
            s_drag_max = 0.0;
        }
        event->accept();
        return;
    }

    if (!is_selecting_range_ && !is_dragging_ && !dragged_clip_ && !trimming_ &&
        !pan_dragging_ && !resizing_track_ && !is_scrubbing_ && !volume_drag_armed_ &&
        !transition_press_armed_ && !transition_editor_.dragging()) {
        update_hover_row(mapToScene(event->pos()));
    } else if (hover_flat_ != -1) {
        clear_row_highlight(hover_highlight_, hover_flat_);
    }

    QGraphicsView::mouseMoveEvent(event);
}

void TimelineWidget::mouseReleaseEvent(QMouseEvent* event) {
    if (pan_dragging_) {
        pan_dragging_ = false;
        event->accept();
        return;
    }

    if (resizing_track_) {
        resizing_track_ = false;
        resize_edge_ = -1;
        resize_total_ = 0;
        if (track_resize_timer_->isActive()) {
            track_resize_timer_->stop();
            rebuild_timeline();
        }
        event->accept();
        return;
    }

    if (transition_press_armed_) {
        transition_press_armed_ = false;
        event->accept();
        return;
    }

    if (transition_editor_.dragging()) {
        release_transition_handle();
        event->accept();
        return;
    }

    if (volume_drag_armed_ || volume_dragging_) {
        if (volume_dragging_) {
            emit volume_line_committed(volume_drag_db_);
            QToolTip::hideText();
        }
        volume_drag_armed_ = false;
        volume_dragging_ = false;
        volume_drag_clip_ = 0;
        volume_drag_db_ = 0.0f;
        volume_drag_targets_.clear();
        unsetCursor();
        event->accept();
        return;
    }

    if (trimming_ && trimmed_clip_) {
        const double scene_x = mapToScene(event->pos()).x();
        const double edge_x = scene_x - trim_grab_offset_px_;
        int64_t new_edge = std::max<int64_t>(
            0, static_cast<int64_t>(std::floor(
                   (edge_x - kSceneMargin - kTrackHeaderWidth) * frames_per_pixel_)));
        new_edge = snap_trim_edge(new_edge, trimmed_clip_);
        unsetCursor();
        if (new_edge == trim_start_edge_) {
            trimming_ = false;
            trimmed_clip_ = nullptr;
            update_snap_indicator(false, 0);
            event->accept();
            return;
        }
        emit clip_trimmed(trimmed_clip_->clip, trim_edge_, new_edge);
        trimming_ = false;
        trimmed_clip_ = nullptr;
        event->accept();
        return;
    }

    if (is_scrubbing_) {
        const int64_t settled = frame_at_x(event->pos().x());
        if (debug_enabled())
            qDebug() << "timeline: scrub released at frame" << settled;
        is_scrubbing_ = false;
        emit playhead_committed(snap_frame(settled));
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
            snap_enabled_ && fps_ > 0.0, frames_per_pixel_, snap_targets_);

        if (drag_clip_snapshot_.size() >= 2) {
            std::size_t primary_i = 0;
            int64_t primary_orig = 0;
            for (std::size_t si = 0; si < drag_clip_snapshot_.size(); ++si) {
                if (drag_clip_snapshot_[si].id == drag_primary_id_) {
                    primary_i = si;
                    primary_orig = drag_clip_orig_[si];
                    break;
                }
            }
            const int64_t min_seq_orig =
                *std::min_element(drag_clip_orig_.begin(), drag_clip_orig_.end());
            const int64_t batch_delta =
                timeline_drag::clamp_batch_delta(primary_orig, res.new_tl_in, min_seq_orig);
            const int64_t primary_new = std::max<int64_t>(0, primary_orig + batch_delta);
            auto& prim = drag_clip_snapshot_[primary_i];
            const bool track_changed =
                dragged_clip_->track_kind != prim.kind ||
                kind_track_index(dragged_clip_->track_index, v_count) != prim.track_index;
            const bool primary_moved = primary_new != primary_orig || track_changed;
            prim.new_tl_in = primary_new;
            prim.kind = dragged_clip_->track_kind;
            prim.track_index = kind_track_index(dragged_clip_->track_index, v_count);
            std::vector<MovedClip> moved;
            for (std::size_t si = 0; si < drag_clip_snapshot_.size(); ++si) {
                auto& s = drag_clip_snapshot_[si];
                if (si == primary_i) {
                    if (primary_moved) moved.push_back(s);
                } else {
                    s.new_tl_in = timeline_drag::batch_target_tl_in(
                        primary_orig, res.new_tl_in, drag_clip_orig_[si], min_seq_orig);
                    if (s.new_tl_in != drag_clip_orig_[si]) moved.push_back(s);
                }
            }
            if (!moved.empty()) emit clips_moved(std::move(moved));
        } else if (res.changed) {
            const canvas::core::Track::Kind kind = dragged_clip_->track_kind;
            const int per_kind = kind_track_index(dragged_clip_->track_index, v_count);
            emit clip_moved(dragged_clip_->clip, res.new_tl_in, kind, per_kind);
        }
        is_dragging_ = false;
        drag_ctrl_.end();
        snap_targets_.clear();
        dragged_clip_ = nullptr;
        drag_mate_ = nullptr;
        drag_clip_snapshot_.clear();
        drag_clip_orig_.clear();
        drag_primary_id_ = 0;
        event->accept();
        return;
    }

    if (dragged_clip_) {
        is_dragging_ = false;
        drag_ctrl_.end();
        snap_targets_.clear();
        dragged_clip_ = nullptr;
        drag_mate_ = nullptr;
        drag_clip_snapshot_.clear();
        drag_clip_orig_.clear();
        drag_primary_id_ = 0;
        update_snap_indicator(false, 0);
        event->accept();
        return;
    }

    QGraphicsView::mouseReleaseEvent(event);
}

void TimelineWidget::dragEnterEvent(QDragEnterEvent* event) {
    if (event->mimeData()->hasFormat("application/x-eh-media-id") ||
        event->mimeData()->hasFormat("application/x-eh-title") ||
        event->mimeData()->hasFormat("application/x-eh-transition") ||
        event->mimeData()->hasUrls()) {
        event->acceptProposedAction();
    } else {
        event->ignore();
    }
}

void TimelineWidget::dragMoveEvent(QDragMoveEvent* event) {
    if (event->mimeData()->hasFormat("application/x-eh-media-id") ||
        event->mimeData()->hasFormat("application/x-eh-title") ||
        event->mimeData()->hasFormat("application/x-eh-transition") ||
        event->mimeData()->hasUrls()) {
        update_drop_lane(mapToScene(event->position().toPoint()));
        if (event->mimeData()->hasFormat("application/x-eh-media-id")) {
            const int media_id = event->mimeData()->data("application/x-eh-media-id").toInt();
            update_drop_preview(event->position().toPoint(), media_id);
        } else {
            clear_drop_preview();
        }
        event->acceptProposedAction();
    } else {
        event->ignore();
    }
}

void TimelineWidget::dropEvent(QDropEvent* event) {
    if (drop_lane_flat_ != -1) clear_row_highlight(drop_lane_highlight_, drop_lane_flat_);
    clear_drop_preview();
    const int64_t frame = frame_at_x(event->position().toPoint().x());
    const double scene_y = mapToScene(event->position().toPoint()).y();

    if (event->mimeData()->hasFormat("application/x-eh-media-id")) {
        const int media_id = event->mimeData()->data("application/x-eh-media-id").toInt();
        emit media_dropped(media_id, frame, scene_y);
        event->acceptProposedAction();
        return;
    }

    if (event->mimeData()->hasFormat("application/x-eh-title")) {
        emit title_dropped(QString::fromUtf8(event->mimeData()->data("application/x-eh-title")),
                           frame, scene_y);
        event->acceptProposedAction();
        return;
    }

    if (event->mimeData()->hasFormat("application/x-eh-transition")) {
        emit transition_dropped(
            QString::fromUtf8(event->mimeData()->data("application/x-eh-transition")),
            frame, scene_y);
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
            emit media_files_dropped(paths, frame, scene_y);
        }
        event->acceptProposedAction();
        return;
    }

    event->ignore();
}

}