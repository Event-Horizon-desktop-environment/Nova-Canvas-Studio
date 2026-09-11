#include "Widgets/timeline_widget.hpp"
#include "Logging.hpp"

#include "core/timecode.hpp"

#include <QColor>
#include <QFont>
#include <QFontMetrics>
#include <QPen>
#include <QBrush>
#include <QScrollBar>
#include <QPointF>
#include <QRectF>
#include <QLatin1Char>
#include <QLineF>
#include <QString>
#include <QGraphicsItem>
#include <QGraphicsRectItem>
#include <QGraphicsTextItem>
#include <QGraphicsPixmapItem>
#include <QGraphicsBlurEffect>
#include <QGraphicsLineItem>
#include <QGraphicsSimpleTextItem>
#include <QPainterPath>
#include <QPainter>
#include <QPixmap>
#include <QSvgRenderer>

#include <algorithm>
#include <cmath>
#include <chrono>

namespace canvas::gui {

void TimelineWidget::rebuild_timeline() {
    const auto rebuild_t0 = std::chrono::steady_clock::now();
    // Whole-scene teardown+rebuild. This is the dominant timeline UX cost on
    // every edit/scrub/resize: bytes of scene items reallocated per second is
    // the "why is the editor getting sticky" tell.
    static int64_t s_rb_n = 0;
    static auto s_rb_at = std::chrono::steady_clock::now();
    static double s_rb_ms = 0.0, s_rb_max = 0.0;
    ++s_rb_n;
    scene_.clear();
    clip_items_.clear();
    video_track_headers_.clear();
    audio_track_headers_.clear();
    dragged_clip_ = nullptr;
    drag_mate_ = nullptr;
    is_dragging_ = false;
    is_selecting_range_ = false;
    selection_rect_ = nullptr;
    playhead_item_ = nullptr;
    minimap_viewport_ = nullptr;
    minimap_background_ = nullptr;
    timecode_item_ = nullptr;
    // scene_.clear() above deleted the previous pinned ruler/minimap group.
    top_pinned_ = nullptr;
    blade_preview_item_ = nullptr;
    snap_indicator_item_ = nullptr;
    hover_highlight_ = nullptr;
    hover_flat_ = -1;
    drop_lane_highlight_ = nullptr;
    drop_lane_flat_ = -1;

    // scene_.clear() above already deleted live transition-cut-handle items.
    // Drop the dangling pointers WITHOUT deleting again, and reset the editor
    // state so a later hover/commit rebuilds cleanly instead of calling
    // scene_.removeItem() on an item whose scene is null (crash).
    transition_items_.clear();
    transition_bubbles_.clear();
    transition_overlay_ = nullptr;
    transition_icon_ = nullptr;
    transition_editor_.close();

    sync_track_heights();

    // Classic "endless timeline": the scene width is the sequence duration at the
    // current zoom, at least wide enough to fill the viewport, with a generous
    // empty tail so scrolling (and the ruler/grid markers) continues past the
    // last clip.
    const double dur_frames = sequence_ ? static_cast<double>(sequence_->duration_frames()) : 0.0;
    const double content_end = kSceneMargin + kTrackHeaderWidth + dur_frames / frames_per_pixel_;
    const double visible_end = kSceneMargin + viewport()->width();
    const double width = std::max(content_end, visible_end) - kSceneMargin + 400.0;
    const int v_count = sequence_ ? static_cast<int>(sequence_->video_tracks.size()) : 1;
    const int a_count = sequence_ ? static_cast<int>(sequence_->audio_tracks.size()) : 0;
    // With no clips on any track yet the track rows stay hidden and the scene
    // shows the compact empty-state placeholder panel instead.
    const bool has_content = has_timeline_content();
    const double content_height = has_content
        ? tracks_content_height(v_count, a_count)
        : (tracks_stack_top() - static_cast<double>(kSceneMargin)) + kEmptyStateHeight
              + track_v_pad_bottom_;
// Grab-and-follow panning: pulling the divider UP parks the channels flush
    // under the ruler strip (that parked scroll value is the UP LIMIT); pulling
    // it DOWN sinks the channels with the cursor through a small, fixed room, so
    // the timeline hugs its tracks instead of showing an endless void below them.
    const double vp_h = static_cast<double>(viewport()->height());
    pan_down_room_ = kPanDownRoomMin;
    const double height = std::max(content_height, vp_h) + kVerticalPanTailMin;

    scene_.setSceneRect(kSceneMargin, kSceneMargin, width, height);
    scene_.addRect(QRectF(kSceneMargin, kSceneMargin, width, height), QPen(Qt::NoPen),
                   QBrush(QColor(0x11, 0x13, 0x1A)));

    // Ruler + minimap live in a viewport-pinned overlay group: the track content
    // pans underneath while the timecode strip stays glued to the top.
    // scrollContentsBy() re-snaps the group to the current vertical scroll.
    top_pinned_ = new QGraphicsItemGroup();
    top_pinned_->setAcceptedMouseButtons(Qt::NoButton);
    top_pinned_->setHandlesChildEvents(false);
    top_pinned_->setZValue(400);
    scene_.addItem(top_pinned_);

    draw_timecode_bar();
    draw_minimap();
    draw_ruler();
    draw_tracks();
    add_transition_bubbles();
    draw_disabled_marks();
    draw_playhead();
    request_clip_thumbnails();
    update_playhead_position(playhead_frame_);
    apply_selection_highlight();
    // Park the channels under the ruler strip and set that as the UP LIMIT: the
    // scroll range keeps the parked value as its top, pulling down sinks
    // through the whole room below it.
    const int park = static_cast<int>(lround(pan_down_room_ + track_v_pad_top_));
    verticalScrollBar()->setRange(0, std::max(park, 0));
    verticalScrollBar()->setValue(park);
    if (top_pinned_) top_pinned_->setPos(0.0, static_cast<double>(verticalScrollBar()->value()));
    if (debug_enabled()) {
        QString sel;
        for (const auto id : selection_.ids())
            sel += QString::number(static_cast<quint64>(id)) + QLatin1Char(' ');
        qDebug() << "timeline: rebuild finished, re-applied selection ->[" << sel << "]";
    }

    const double rb_ms = std::chrono::duration<double, std::milli>(
                             std::chrono::steady_clock::now() - rebuild_t0).count();
    s_rb_ms += rb_ms;
    s_rb_max = std::max(s_rb_max, rb_ms);
    const auto rb_now = std::chrono::steady_clock::now();
    if (s_rb_n == 1 || rb_now - s_rb_at >= std::chrono::seconds(1)) {
        s_rb_at = rb_now;
        qWarning() << "[ui:timeline] rebuild ms_avg=" << QString::number(s_rb_ms / s_rb_n, 'f', 2)
                   << "ms_last=" << QString::number(rb_ms, 'f', 2)
                   << "ms_max=" << QString::number(s_rb_max, 'f', 2)
                   << "n=" << s_rb_n
                   << "clips=" << (sequence_ ? static_cast<int>(clip_items_.size()) : 0)
                   << "items=" << scene_.items().size();
        s_rb_n = 0;
        s_rb_ms = 0.0;
        s_rb_max = 0.0;
    }
}

void TimelineWidget::draw_timecode_bar() {
    // Pinned "current time" bar: a persistent playhead readout at the very top of
    // the timeline (above the minimap), refreshed whenever the playhead moves.
    auto* bg = scene_.addRect(
        QRectF(kSceneMargin, 0.0, scene_.sceneRect().width(), kTimecodeBarHeight),
        QPen(QColor(0x2A, 0x2F, 0x3C)), QBrush(QColor(0x1A, 0x1D, 0x27)));
    bg->setAcceptedMouseButtons(Qt::NoButton);
    top_pinned_->addToGroup(bg);

    timecode_item_ = scene_.addText(timecode(playhead_frame_, fps_));
    timecode_item_->setAcceptedMouseButtons(Qt::NoButton);
    timecode_item_->setDefaultTextColor(QColor(0xE8, 0xEC, 0xF2));
    QFont f = timecode_item_->font();
    f.setFamily(QStringLiteral("Monospace"));
    f.setBold(true);
    f.setPointSizeF(9.5);
    timecode_item_->setFont(f);
    timecode_item_->setPos(kSceneMargin + 6.0, 4.5);
    top_pinned_->addToGroup(timecode_item_);
}

void TimelineWidget::draw_minimap() {
    const double left = kSceneMargin;
    const double top = kTimecodeBarHeight + kSceneMargin;
    const double width = scene_.sceneRect().width();
    minimap_background_ = scene_.addRect(
        QRectF(left, top, width, kMinimapHeight),
        QPen(QColor(0x2A, 0x2F, 0x3C)), QBrush(QColor(0x1A, 0x1D, 0x27)));
    if (top_pinned_) {
        minimap_background_->setAcceptedMouseButtons(Qt::NoButton);
        top_pinned_->addToGroup(minimap_background_);
    }

    const int64_t dur = std::max<int64_t>(sequence_ ? sequence_->duration_frames() : 1, 1);
    const double strip_w = width - kTrackHeaderWidth;
    if (!sequence_) return;

    for (const auto& track : sequence_->video_tracks) {
        for (const auto& clip : track.clips) {
            const double cw = std::max(1.0, strip_w * clip.duration() / dur);
            const double cx = left + kTrackHeaderWidth + strip_w * (clip.tl_in / static_cast<double>(dur));
            auto* mini = scene_.addRect(QRectF(cx, top + 2, cw, kMinimapHeight - 4), QPen(Qt::NoPen),
                                        QBrush(QColor(0x3B, 0x82, 0xF6)));
            if (top_pinned_) {
                mini->setAcceptedMouseButtons(Qt::NoButton);
                top_pinned_->addToGroup(mini);
            }
        }
    }

    const double scene_visible_frames = scene_.sceneRect().width() * frames_per_pixel_;
    const double vp_start = horizontalScrollBar()->value() * frames_per_pixel_;
    const double vp_x = left + kTrackHeaderWidth + strip_w * (vp_start / dur);
    const double vp_w = std::max(4.0, strip_w * (scene_visible_frames / dur));
    minimap_viewport_ = scene_.addRect(
        QRectF(vp_x, top, std::min(vp_w, width - kTrackHeaderWidth - vp_x), kMinimapHeight),
        QPen(QColor(0x6F, 0xB0, 0xFF)), QBrush(QColor(0x3B, 0x82, 0xF6, 70)));
    minimap_viewport_->setZValue(50);
    if (top_pinned_) {
        minimap_viewport_->setAcceptedMouseButtons(Qt::NoButton);
        top_pinned_->addToGroup(minimap_viewport_);
    }
}

void TimelineWidget::update_minimap_viewport() {
    if (!minimap_viewport_ || !sequence_) return;
    const double dur = std::max<int64_t>(sequence_->duration_frames(), 1);
    const double left = kSceneMargin;
    const double width = scene_.sceneRect().width();
    const double strip_w = width - kTrackHeaderWidth;
    const double top = kTimecodeBarHeight + kSceneMargin;
    const double scene_visible_frames = std::max(1.0, scene_.sceneRect().width() * frames_per_pixel_);
    const double vp_start = std::max(0.0, horizontalScrollBar()->value() * frames_per_pixel_);
    const double vp_x = left + kTrackHeaderWidth + strip_w * (vp_start / dur);
    const double vp_w = std::max(4.0, strip_w * (std::min(scene_visible_frames, dur) / dur));
    const double max_x = width - kTrackHeaderWidth;
    const double x = std::min(vp_x + vp_w, left + strip_w);
    minimap_viewport_->setRect(QRectF(std::min(vp_x, max_x), top,
                                      std::min(vp_w, std::max(4.0, max_x - std::min(vp_x, max_x))),
                                      kMinimapHeight));
}

void TimelineWidget::draw_ruler() {
    const double top = kMinimapHeight + kSceneMargin + kTimecodeBarHeight;
    const double scene_bottom = scene_.sceneRect().bottom();
    auto* bg = scene_.addRect(
        QRectF(kSceneMargin, top, scene_.sceneRect().width(), kRulerHeight),
        QPen(QColor(0x2A, 0x2F, 0x3C)), QBrush(QColor(0x1A, 0x1D, 0x27)));
    if (top_pinned_) {
        bg->setAcceptedMouseButtons(Qt::NoButton);
        top_pinned_->addToGroup(bg);
    }

    const double fpp = frames_per_pixel_;

    // Minor ticks (short marks inside the ruler only), spaced ~8-18px apart.
    double step = 1.0;
    while (step / fpp < 8.0) step *= 2.0;
    while (step / fpp > 18.0) step /= 2.0;

    // Major ticks (full-height gridline + timecode label) spaced so labels
    // don't collide (~90px on screen).
    const int major_mod = std::max(1, static_cast<int>(std::ceil(90.0 / (step / fpp))));

    const double left = scene_.sceneRect().left();
    const double right = scene_.sceneRect().right();
    const QPen grid_pen(QColor(0x2A, 0x2F, 0x3C, 110));
    const QPen minor_pen(QColor(0x9A, 0xA0, 0xB0, 90));
    const QPen major_pen(QColor(0x9A, 0xA0, 0xB0, 170));
    int tick_i = 0;
    for (double frame = 0.0; left + kTrackHeaderWidth + frame / fpp <= right; frame += step, ++tick_i) {
        const double x = kSceneMargin + kTrackHeaderWidth + frame / fpp;
        const bool major = (tick_i % major_mod) == 0;
        if (major) {
            // Full-height gridline down through every track — lives in the SCENE
            // (unpinned) so it never shifts with vertical pannin.
            scene_.addLine(QLineF(x, top, x, scene_bottom), grid_pen);
            // The ruler's own tick + timecode label are pinned with the strip.
            auto* tick = scene_.addLine(QLineF(x, top, x, top + (frame == 0.0 ? 16.0 : 12.0)), major_pen);
            if (top_pinned_) {
                tick->setAcceptedMouseButtons(Qt::NoButton);
                top_pinned_->addToGroup(tick);
            }
            auto* label = scene_.addText(timecode(static_cast<int64_t>(frame), fps_));
            label->setDefaultTextColor(QColor(0x9A, 0xA0, 0xB0));
            QFont f = label->font();
            f.setPointSizeF(8);
            label->setFont(f);
            label->setPos(QPointF(x + 2, top + 2));
            if (top_pinned_) {
                label->setAcceptedMouseButtons(Qt::NoButton);
                top_pinned_->addToGroup(label);
            }
        } else {
            // Small minor tick in the ruler only (no gridline).
            auto* tick = scene_.addLine(QLineF(x, top + kRulerHeight - 8, x, top + kRulerHeight), minor_pen);
            if (top_pinned_) {
                tick->setAcceptedMouseButtons(Qt::NoButton);
                top_pinned_->addToGroup(tick);
            }
        }
    }

    // Bookmarks (Mark > Add Marker, M): small amber diamonds in the ruler.
    if (sequence_) {
        const QPen bk_pen(QColor(0xFF, 0xC1, 0x07), 1);
        const QBrush bk_brush(QColor(0xFF, 0xC1, 0x07));
        for (const auto& b : sequence_->bookmarks) {
            const double bx = kSceneMargin + kTrackHeaderWidth + b.frame / fpp;
            auto* bk = scene_.addRect(QRectF(bx - 3, top + kRulerHeight - 14, 6, 6),
                                      bk_pen, bk_brush);
            if (top_pinned_) {
                bk->setAcceptedMouseButtons(Qt::NoButton);
                top_pinned_->addToGroup(bk);
            }
        }
    }

    // Continuous vertical divider between the header column and the scrollable
    // track area, running through every track (unpinned).
    scene_.addLine(QLineF(kSceneMargin + kTrackHeaderWidth, top, kSceneMargin + kTrackHeaderWidth, scene_bottom),
                   QPen(QColor(0x2A, 0x2F, 0x3C)));
}

namespace {

// Renders one bundled SVG icon to a tinted pixmap and places it as a
// QGraphicsPixmapItem at (x, y) in the scene, returning the item.
QGraphicsPixmapItem* add_icon(QGraphicsScene& scene, const QString& name, double x, double y,
                              const QColor& color, int size = 14) {
    QPixmap pm(size, size);
    pm.fill(Qt::transparent);
    QSvgRenderer renderer(QStringLiteral(":/icons/%1.svg").arg(name));
    QPainter p(&pm);
    renderer.render(&p, QRectF(0, 0, size, size));
    p.end();
    QPixmap tinted(size, size);
    tinted.fill(color);
    QPainter tp(&tinted);
    tp.setCompositionMode(QPainter::CompositionMode_SourceIn);
    tp.drawPixmap(0, 0, pm);
    tp.end();
    auto* item = scene.addPixmap(tinted);
    item->setPos(QPointF(x, y));
    item->setZValue(3);
    item->setAcceptedMouseButtons(Qt::NoButton);
    return item;
}

// The pieces of a track-type badge (V1/A1 pill + its label).
struct TrackBadge {
    QGraphicsPathItem* pill = nullptr;
    QGraphicsSimpleTextItem* text = nullptr;
};

// Builds the track-type badge: a quiet raised pill carrying a colored bold
    // label dead-centered via tight font metrics so padding can never shift it
    // off-center. The label color (not the fill) is the track-type identity:
    // gold=video, cyan=dialogue audio, amber=music audio — same code as the
    // far-left accent bar, so the header reads as one coherent system.
    TrackBadge add_badge(QGraphicsScene& scene, const QString& text, const QPointF& top_left,
                     const QColor& fill, const QColor& text_color) {
    QFont bf;
    bf.setPointSizeF(8);
    bf.setBold(true);
    const QFontMetrics bfm(bf);
    const qreal pad = 6.0;
    const qreal h = 15.0;
    const qreal w = bfm.horizontalAdvance(text) + pad * 2.0;
    const QRectF r(top_left.x(), top_left.y(), w, h);

    QPainterPath pill;
    pill.addRoundedRect(r, 3.0, 3.0);
    auto* pill_item = scene.addPath(pill, QPen(Qt::NoPen), QBrush(fill));
    pill_item->setZValue(2);
    pill_item->setAcceptedMouseButtons(Qt::NoButton);

    auto* text_item = scene.addSimpleText(text);
    text_item->setBrush(text_color);
    text_item->setFont(bf);
    text_item->setAcceptedMouseButtons(Qt::NoButton);
    // Dead-center on the pill using the simple-text item's tight glyph box, so
    // the label reads optically centered.
    const QRectF br = text_item->boundingRect();
    text_item->setPos(r.center() - br.center());
    text_item->setZValue(3);
    return {pill_item, text_item};
}

}  // namespace

void TimelineWidget::draw_tracks() {
    const int v_count = sequence_ ? static_cast<int>(sequence_->video_tracks.size()) : 1;
    const int a_count = sequence_ ? static_cast<int>(sequence_->audio_tracks.size()) : 0;

    const double left = kSceneMargin;
    constexpr QColor kIconColor(0x9A, 0xA0, 0xB0);       // muted ink for icons
    constexpr QColor kIconColorDim(0x5F, 0x65, 0x77);    // faint ink for dim icons
    constexpr QColor kIconAccent(0x4C, 0x92, 0xFF);      // active-state ink (lock/mute/solo)

    // With no clips yet the header column and rows stay hidden; a single
    // placeholder panel explains the empty state instead.
    if (!has_timeline_content()) {
        draw_empty_state();
        return;
    }

    // The whole track-header column is ONE continuous panel: a single
    // background rect spanning all rows plus a vertical divider at its right
    // edge. Rows no longer paint their own background — only the accent bar,
    // badge and icons are per-row.
    const double h_top = tracks_stack_top();
    const double h_bot = tracks_stack_bottom(v_count, a_count);
    scene_.addRect(QRectF(left, h_top, kTrackHeaderWidth, h_bot - h_top),
                   QPen(Qt::NoPen), QBrush(QColor(0x16, 0x1A, 0x22)))->setZValue(0);
    scene_.addLine(QLineF(left + kTrackHeaderWidth, h_top,
                          left + kTrackHeaderWidth, h_bot),
                   QPen(QColor(0x2A, 0x2F, 0x3C)))->setZValue(0);

    for (int i = 0; i < v_count; ++i) {
        const double y = track_top(i, v_count);
        const double th = track_height(i, v_count);
        QString tname = QString::fromStdString(sequence_->video_tracks[i].name);

        // Track-header content is tiered so it always fits whatever row height the
        // user dragged to: tall rows get the full two-tier layout, compact rows
        // collapse the icons onto one centered row and drop the label/count.
        const bool full = th >= 52.0;

        // Colored track-type badge (top-left of the header): quiet raised pill
        // with a gold label (video identity lives in the label color, not the pill).
        const QString badge_text = tname.isEmpty() ? QStringLiteral("V%1").arg(i + 1) : tname;
        const double badge_top = full ? y + 8.0 : y + (th - 15.0) / 2.0;
<<<<<<< Updated upstream
        TrackBadge tb = add_badge(scene_, badge_text, QPointF(left + 8, badge_top), QColor(0x3B, 0x82, 0xF6));
=======
        TrackBadge tb = add_badge(scene_, badge_text, QPointF(left + 8, badge_top),
                                  t.surface_highest, t.accent);
>>>>>>> Stashed changes
        auto* badge_pill = tb.pill;
        auto* badge = tb.text;

        // Thin full-height accent bar on the far-left edge (per-track-type code).
<<<<<<< Updated upstream
        scene_.addRect(QRectF(left, y + 1, 3, th - 2), QPen(Qt::NoPen),
                       QBrush(QColor(0x3B, 0x82, 0xF6)))->setZValue(1);
=======
        scene_.addRect(QRectF(left, y + 1, 4, th - 2), QPen(Qt::NoPen),
                       QBrush(t.accent))->setZValue(1);
>>>>>>> Stashed changes

        // Reorder / lock / view-mode icon row, right-aligned in the header.
        QGraphicsPixmapItem* lock_icon = nullptr;
        QGraphicsPixmapItem* view_mode_icon = nullptr;
        const bool vlocked = sequence_->video_tracks[i].locked;
        if (full) {
            add_icon(scene_, QStringLiteral("reorder"), left + kTrackHeaderWidth - 44, y + 10, kIconColorDim, 13);
            lock_icon = add_icon(scene_, vlocked ? QStringLiteral("lock") : QStringLiteral("unlock"),
                                 left + kTrackHeaderWidth - 26, y + 9,
                                 vlocked ? kIconAccent : kIconColor);
            view_mode_icon = add_icon(scene_, QStringLiteral("track_viewmode"), left + kTrackHeaderWidth - 44, y + 21, kIconColor);
        } else {
            const double ic_y = y + (th - 13.0) / 2.0;
            add_icon(scene_, QStringLiteral("reorder"), left + kTrackHeaderWidth - 53, ic_y, kIconColorDim, 13);
            lock_icon = add_icon(scene_, vlocked ? QStringLiteral("lock") : QStringLiteral("unlock"),
                                 left + kTrackHeaderWidth - 37, ic_y,
                                 vlocked ? kIconAccent : kIconColor);
            view_mode_icon = add_icon(scene_, QStringLiteral("track_viewmode"), left + kTrackHeaderWidth - 21, ic_y, kIconColor);
        }

        // "Video N" label. Hidden below 56px rows so it can never bleed out of the
        // header into the track below (fixed offsets once did).
        QGraphicsTextItem* label = nullptr;
        if (full && th >= 56.0) {
            label = scene_.addText(QStringLiteral("Video %1").arg(i + 1));
            label->setDefaultTextColor(QColor(0x9A, 0xA0, 0xB0));
            QFont lf = label->font();
            lf.setPointSizeF(9);
            lf.setBold(true);
            label->setFont(lf);
            label->setPos(QPointF(left + 8, y + 28));
            label->setZValue(1);
        }

        // "N Clips" count, bottom-anchored and clamped so a short row can never push
        // it out of the header (the old fixed offset ghosted onto the track below).
        QGraphicsTextItem* count = nullptr;
        if (full) {
            count = scene_.addText(QStringLiteral("%1 Clips").arg(sequence_->video_tracks[i].clips.size()));
            count->setDefaultTextColor(QColor(0x5F, 0x65, 0x77));
            QFont cf = count->font();
            cf.setPointSizeF(8);
            count->setFont(cf);
            count->setPos(QPointF(left + 8, std::min(y + 44.0, y + th - 14.0)));
            count->setZValue(1);
        }
        TrackHeader header;
        header.background = nullptr;  // one column-wide background, not per-row ones
        header.label = label;
        header.badge = badge;
        header.badge_pill = badge_pill;
        header.count = count;
        header.lock_icon = lock_icon;
        header.view_mode_icon = view_mode_icon;
        video_track_headers_.push_back(header);

        const canvas::core::Track* track = &sequence_->video_tracks[i];
        const int track_index = i;
        for (const auto& clip : track->clips) {
            const double cx = left + kTrackHeaderWidth + clip.tl_in / frames_per_pixel_;
            const double cw = clip.duration() / frames_per_pixel_;
            const double cy = y + 2;
            const double ch = th - 6;
            const double label_h = std::min(kClipLabelHeight, ch * 0.35);
            const double body_h = ch - label_h;

            // Semi-rounded flat clip block; neutral cool outline when unselected,
            // blue (selection) applied on top by apply_selection_highlight.
            auto* rect = scene_.addRect(
                QRectF(0, 0, cw, ch), QPen(Qt::NoPen), QBrush(Qt::transparent));
            rect->setPos(QPointF(cx, cy));
            rect->setAcceptedMouseButtons(Qt::NoButton);
            // Flat fill exactly abutting neighbours (a centred pen on the bounding
            // rect would overhang into the adjacent clip at a cut). The outline
            // stroke is inset by pen/2 so its outer edge lands on the clip's edge.
<<<<<<< Updated upstream
=======
            // Clip colour recolours the shell itself (Resolve-style): the whole
            // body takes the swatch as its real fill — the filmstrip cells ride
            // ABOVE it (z 1) so thumbnails are never covered by the colour.
            const QColor cc_v = clip_color_for(clip.clip_color);
            // Shell gets the swatch blended 65% onto the normal dark body fill so
            // a coloured clip reads as a bold tint, not a flat neon slab.
            const QColor shell_v =
                cc_v.isValid() ? blend_colors(cc_v, t.clip_video, 0.65) : t.clip_video;
            // 1px baseline shadow: a rounded copy offset down by kClipShadowOffset
            // so the clip casts a soft drop onto the track. The ClipClipGroup clips
            // it to this clip's own rect, so only a hairline under the body shows.
            auto* shadow = scene_.addPath(
                rounded_rect_path(QRectF(0, kClipShadowOffset, cw, ch), 6),
                QPen(Qt::NoPen), QBrush(t.clip_shadow));
            shadow->setPos(QPointF(cx, cy));
            shadow->setAcceptedMouseButtons(Qt::NoButton);
>>>>>>> Stashed changes
            auto* shell = scene_.addPath(
                rounded_rect_path(QRectF(0, 0, cw, ch), 6),
                QPen(Qt::NoPen), QBrush(QColor(0x2A, 0x35, 0x40)));
            shell->setPos(QPointF(cx, cy));
            shell->setAcceptedMouseButtons(Qt::NoButton);
            auto* outline = scene_.addPath(
                rounded_rect_path(QRectF(kClipOutlineW / 2.0, kClipOutlineW / 2.0,
                                         cw - kClipOutlineW, ch - kClipOutlineW), 6),
<<<<<<< Updated upstream
                QPen(QColor(0x4C, 0x92, 0xFF), kClipOutlineW), QBrush(Qt::NoBrush));
=======
                QPen(t.clip_border_video, kClipOutlineW), QBrush(Qt::NoBrush));
>>>>>>> Stashed changes
            outline->setPos(QPointF(cx, cy));
            outline->setZValue(2.0);
            outline->setAcceptedMouseButtons(Qt::NoButton);

            // Semi-rounded label bar: flat muted blue strip with link icon + filename,
            // hugging the bottom of the clip.
            const QColor kLabelBlue(0x3D, 0x5A, 0x73);
            auto* label_bar = scene_.addPath(
                rounded_rect_path(QRectF(0, body_h, cw, label_h), 6),
                QPen(Qt::NoPen), QBrush(kLabelBlue));
            label_bar->setPos(QPointF(cx, cy));
            label_bar->setZValue(0);

            QGraphicsTextItem* text = nullptr;
            if (cw > 60.0) {
                add_icon(scene_, QStringLiteral("link"), cx + 4, cy + body_h + 3.5, QColor(0xD0, 0xD8, 0xDF), 11);
                text = scene_.addText(QString::fromStdString(clip.name));
                text->setDefaultTextColor(QColor(0xE3, 0xE8, 0xED));
                QFont tf = text->font();
                tf.setPointSizeF(7.5);
                text->setFont(tf);
                text->setPos(QPointF(cx + 17, cy + body_h + 1));
                text->setZValue(2);
                text->setAcceptedMouseButtons(Qt::NoButton);
            }

            ClipItem item;
            item.rect = rect;
            item.shell = shell;
            item.outline = outline;
            item.label_bar = label_bar;
            item.text = text;
            item.clip = &clip;
            item.track_index = track_index;
            item.track_kind = canvas::core::Track::Kind::Video;

            // Clip every child to the clip's own rect so an abutting clip's
            // content (filmstrip, outline) can never overlap its neighbour.
            auto* clip_group = new ClipClipGroup(QRectF(cx, cy, cw, ch));
            scene_.addItem(clip_group);
            for (QGraphicsItem* child : std::initializer_list<QGraphicsItem*>{shadow, rect, shell, outline, label_bar, text})
                if (child) child->setParentItem(clip_group);

            // OUT-transition badge: a small SVG mark near the clip's trailing edge for
            // an outgoing transition, or a fade-to-black at the tail. Audio
            // fades get their own teal badge in the audio loop below.
            if (clip.has_transition_out() &&
                !canvas::core::is_audio_transition(clip.transition_out) && cw > 40.0) {
                add_icon(scene_, QStringLiteral("transition_out"), cx + cw - 16, cy + 3,
                         QColor(0xF2, 0xA9, 0x4A), 12);
            }

            // IN-transition badge: marks a fade-in-from-black at the clip's head.
            if (clip.has_transition_in() &&
                !canvas::core::is_audio_transition(clip.transition_in) && cw > 40.0) {
                add_icon(scene_, QStringLiteral("transition_in"), cx + 4, cy + 3,
                         QColor(0x4A, 0xA9, 0xF2), 12);
            }

            // Filmstrip thumbnails butted together to fill the body region edge
            // to edge. The pitch is 42px/cell at most: long clips at extreme zoom
            // would otherwise queue hundreds of decodes per zoom step, so cap the
            // cell count (cells then just widen, the strip stays fully covered).
            const int num_cells =
                std::min(std::max(1, static_cast<int>(cw / kFilmstripCellWidth)), 320);
            const double cell_w = cw / num_cells;
            for (int c = 0; c < num_cells; ++c) {
                auto* cell = scene_.addPixmap(QPixmap());
                cell->setPos(cx + c * cell_w, cy + 2);
                cell->setZValue(1);
                cell->setAcceptedMouseButtons(Qt::NoButton);
                item.cells.push_back(ClipCell{cell, 0});
            }
            // Reparent the filmstrip into the clip group so the thumbnails clip to the
            // clip's rect AND travel with it while dragging.
            for (const auto& cell : item.cells)
                if (cell.item) cell.item->setParentItem(clip_group);
            clip_items_.push_back(std::move(item));
        }
    }

    for (int i = 0; i < a_count; ++i) {
        const double y = track_top(v_count + i, v_count);
        const double th = track_height(v_count + i, v_count);
        const QString tname = QString::fromStdString(sequence_->audio_tracks[i].name);
        const QString badge_text = tname.isEmpty() ? QStringLiteral("A%1").arg(i + 1) : tname;
        // Audio badge labels carry the role tint (amber for music-role tracks
        // A3/A4, cyan for dialogue); the pill itself stays the quiet raised
        // surface so the label color is the identity signal.
        const bool is_music_role = i >= 2;
        const QColor role_color = is_music_role ? QColor(0xC9, 0x86, 0x3A) : QColor(0x2E, 0x8F, 0xC0);
        const bool full = th >= 52.0;

        const double badge_top = full ? y + 8.0 : y + (th - 15.0) / 2.0;
        // Quiet raised pill; the label carries the role identity (cyan=dialogue,
        // amber=music) so the header reads as one system with the accent bar.
        TrackBadge tb = add_badge(scene_, badge_text, QPointF(left + 8, badge_top),
                                  t.surface_highest,
                                  is_music_role ? t.accent_text : t.playhead);
        auto* badge_pill = tb.pill;
        auto* badge = tb.text;

        // Thin full-height accent bar on the far-left edge. Music-role tracks
        // (A3/A4) get the warm orange mark; dialogue tracks get the audio cyan.
        scene_.addRect(QRectF(left, y + 1, 4, th - 2), QPen(Qt::NoPen),
                       QBrush(role_color))->setZValue(1);

        QGraphicsPixmapItem* lock_icon = nullptr;
        QGraphicsPixmapItem* solo_icon = nullptr;
        QGraphicsPixmapItem* mute_icon = nullptr;
        const auto& hdr_track = sequence_->audio_tracks[i];
        const bool alocked = hdr_track.locked;
        const bool asolo = hdr_track.solo;
        const bool amuted = hdr_track.muted;
        if (full) {
            lock_icon = add_icon(scene_, alocked ? QStringLiteral("lock") : QStringLiteral("unlock"),
                                 left + kTrackHeaderWidth - 26, y + 9,
                                 alocked ? kIconAccent : kIconColor);
            solo_icon = add_icon(scene_, QStringLiteral("solo"), left + kTrackHeaderWidth - 44, y + 20,
                                 asolo ? QColor(0xF2, 0xA9, 0x3C) : kIconColor);
            mute_icon = add_icon(scene_, amuted ? QStringLiteral("mute") : QStringLiteral("volume"),
                                 left + kTrackHeaderWidth - 26, y + 20,
                                 amuted ? QColor(0xEF, 0x44, 0x44) : kIconColor);
        } else {
            const double ic_y = y + (th - 13.0) / 2.0;
            lock_icon = add_icon(scene_, alocked ? QStringLiteral("lock") : QStringLiteral("unlock"),
                                 left + kTrackHeaderWidth - 53, ic_y,
                                 alocked ? kIconAccent : kIconColor);
            solo_icon = add_icon(scene_, QStringLiteral("solo"), left + kTrackHeaderWidth - 37, ic_y,
                                 asolo ? QColor(0xF2, 0xA9, 0x3C) : kIconColor);
            mute_icon = add_icon(scene_, amuted ? QStringLiteral("mute") : QStringLiteral("volume"),
                                 left + kTrackHeaderWidth - 21, ic_y,
                                 amuted ? QColor(0xEF, 0x44, 0x44) : kIconColor);
        }

        // "Audio N" label (tall rows only; see the video branch).
        QGraphicsTextItem* label = nullptr;
        if (full && th >= 56.0) {
            label = scene_.addText(QStringLiteral("Audio %1").arg(i + 1));
            label->setDefaultTextColor(QColor(0x9A, 0xA0, 0xB0));
            QFont lf = label->font();
            lf.setPointSizeF(9);
            lf.setBold(true);
            label->setFont(lf);
            label->setPos(QPointF(left + 8, y + 28));
            label->setZValue(1);
        }

        // "N Clips" count, bottom-anchored/clamped (see video branch).
        QGraphicsTextItem* count = nullptr;
        if (full) {
            count = scene_.addText(QStringLiteral("%1 Clips").arg(sequence_->audio_tracks[i].clips.size()));
            count->setDefaultTextColor(QColor(0x5F, 0x65, 0x77));
            QFont cf = count->font();
            cf.setPointSizeF(8);
            count->setFont(cf);
            count->setPos(QPointF(left + 8, std::min(y + 44.0, y + th - 14.0)));
            count->setZValue(1);
        }

        QGraphicsTextItem* channel_badge = nullptr;
        if (full) {
            channel_badge = scene_.addText(QStringLiteral("2.0"));
            channel_badge->setDefaultTextColor(QColor(0x5F, 0x65, 0x77));
            QFont chf = channel_badge->font();
            chf.setPointSizeF(7);
            channel_badge->setFont(chf);
            channel_badge->setPos(QPointF(left + kTrackHeaderWidth - 24, y + th - 16));
            channel_badge->setZValue(1);
        }

        TrackHeader header;
        header.background = nullptr;  // one column-wide background, not per-row ones
        header.label = label;
        header.badge = badge;
        header.badge_pill = badge_pill;
        header.count = count;
        header.lock_icon = lock_icon;
        header.solo_icon = solo_icon;
        header.mute_icon = mute_icon;
        header.channel_badge = channel_badge;
        audio_track_headers_.push_back(header);

        const canvas::core::Track* track = &sequence_->audio_tracks[i];
        const int track_index = i;
        for (const auto& clip : track->clips) {
            const double cx = left + kTrackHeaderWidth + clip.tl_in / frames_per_pixel_;
            const double cw = clip.duration() / frames_per_pixel_;
            const double cy = y + 3;
            const double ch = th - 6;
            const double label_h = std::min(kClipLabelHeight, ch * 0.35);
            const double body_h = ch - label_h;

            // Semi-rounded flat clip block; neutral green-cast outline when unselected,
            // amber (selection) applied on top by apply_selection_highlight; sage fill.
            auto* rect = scene_.addRect(
                QRectF(0, 0, cw, ch), QPen(Qt::NoPen), QBrush(Qt::transparent));
            rect->setPos(QPointF(cx, cy));
            rect->setAcceptedMouseButtons(Qt::NoButton);
            // Flat sage fill abutting neighbours exactly; the outline stroke is
            // inset by pen/2 so it never overhangs into an abutting clip.
<<<<<<< Updated upstream
=======
            // Clip colour recolours the shell itself (see video branch): the
            // waveform rides ABOVE it (z 1) and is never covered.
            const QColor cc_a = clip_color_for(clip.clip_color);
            const QColor shell_a =
                cc_a.isValid() ? blend_colors(cc_a, t.clip_audio, 0.65) : t.clip_audio;
            auto* shadow = scene_.addPath(
                rounded_rect_path(QRectF(0, kClipShadowOffset, cw, ch), 6),
                QPen(Qt::NoPen), QBrush(t.clip_shadow));
            shadow->setPos(QPointF(cx, cy));
            shadow->setAcceptedMouseButtons(Qt::NoButton);
>>>>>>> Stashed changes
            auto* shell = scene_.addPath(
                rounded_rect_path(QRectF(0, 0, cw, ch), 6),
                QPen(Qt::NoPen), QBrush(QColor(0x5A, 0x6B, 0x4A)));
            shell->setPos(QPointF(cx, cy));
            shell->setAcceptedMouseButtons(Qt::NoButton);
            auto* outline = scene_.addPath(
                rounded_rect_path(QRectF(kClipOutlineW / 2.0, kClipOutlineW / 2.0,
                                         cw - kClipOutlineW, ch - kClipOutlineW), 6),
<<<<<<< Updated upstream
                QPen(QColor(0x4C, 0x92, 0xFF), kClipOutlineW), QBrush(Qt::NoBrush));
=======
                QPen(t.clip_border_audio, kClipOutlineW), QBrush(Qt::NoBrush));
>>>>>>> Stashed changes
            outline->setPos(QPointF(cx, cy));
            outline->setZValue(2.0);
            outline->setAcceptedMouseButtons(Qt::NoButton);

            QGraphicsTextItem* text = nullptr;
            if (cw > 60.0) {
                text = scene_.addText(QString::fromStdString(clip.name));
                text->setDefaultTextColor(QColor(0xE3, 0xE8, 0xED));
                QFont tf = text->font();
                tf.setPointSizeF(7.5);
                text->setFont(tf);
                text->setPos(QPointF(cx + 5, cy + body_h + 1));
                text->setZValue(2);
                text->setAcceptedMouseButtons(Qt::NoButton);
            }
            auto* wf = scene_.addPixmap(QPixmap());
            wf->setPos(cx + 2, cy + 2);
            wf->setZValue(1);
            wf->setAcceptedMouseButtons(Qt::NoButton);

            // AUDIO fade indicator: audio transitions get their own teal badge so a
            // linked video+audio pair carries the indicator on BOTH clips.
            if (clip.has_transition_out() &&
                canvas::core::is_audio_transition(clip.transition_out) && cw > 40.0) {
                add_icon(scene_, QStringLiteral("transition_out"), cx + cw - 16, cy + 3,
                         QColor(0x4A, 0xC8, 0x9A), 12);
            }
            if (clip.has_transition_in() &&
                canvas::core::is_audio_transition(clip.transition_in) && cw > 40.0) {
                add_icon(scene_, QStringLiteral("transition_in"), cx + 4, cy + 3,
                         QColor(0x4A, 0xC8, 0x9A), 12);
            }

            ClipItem item;
            item.rect = rect;
            item.shell = shell;
            item.outline = outline;
            item.label_bar = nullptr;
            item.text = text;
            item.cells.push_back(ClipCell{wf, 0});
            item.clip = &clip;
            item.track_index = v_count + track_index;
            item.track_kind = canvas::core::Track::Kind::Audio;

            // Clip every child to the clip's own rect (see video branch).
            auto* clip_group = new ClipClipGroup(QRectF(cx, cy, cw, ch));
            scene_.addItem(clip_group);
<<<<<<< Updated upstream
            for (QGraphicsItem* child : std::initializer_list<QGraphicsItem*>{rect, shell, outline, text})
=======
            for (QGraphicsItem* child : std::initializer_list<QGraphicsItem*>{shadow, rect, shell, outline, text, vol})
>>>>>>> Stashed changes
                if (child) child->setParentItem(clip_group);
            for (const auto& cell : item.cells)
                if (cell.item) cell.item->setParentItem(clip_group);
            clip_items_.push_back(std::move(item));
        }
    }

    // Rows separated by thin horizontal dividers spanning the full width. The
    // hairline below V1 is skipped: the Video/Audio divider band takes over.
    const int total_t = v_count + a_count;
    const double right_edge = scene_.sceneRect().right();
    const QPen row_pen(QColor(0x23, 0x28, 0x33));
    for (int t = 0; t < total_t; ++t) {
        if (t == v_count - 1 && a_count > 0) continue;  // video/audio boundary
        const double bottom = track_top(t, v_count) + track_height(t, v_count);
        scene_.addLine(QLineF(left, bottom, right_edge, bottom), row_pen);
    }

    if (v_count > 0 && a_count > 0) {
        // Dedicated full-width Video/Audio divider band, sealed off from V1 above
        // and A1 below: the band's top/bottom edges are the resize boundaries
        // and the strip between them is the grab-and-scroll pan handle. A
        // centered accent line + grip pill mark the grab zone exactly.
        const double band_top = edge_y(v_count, v_count, a_count);
        const double band_bot = edge_y(v_count + 1, v_count, a_count);
        const double band_w = std::max(0.0, right_edge - left);
        scene_.addRect(QRectF(left, band_top, band_w, band_bot - band_top),
                       QPen(Qt::NoPen), QBrush(QColor(0x14, 0x17, 0x1F)))->setZValue(0);
        const double mid = (band_top + band_bot) / 2.0;
        scene_.addLine(QLineF(left, mid - 1, right_edge, mid - 1),
                       QPen(QColor(0x4C, 0x92, 0xFF), 2.0))->setZValue(1);
        auto* grip = scene_.addRect(QRectF(left + (kTrackHeaderWidth - 34.0) / 2.0,
                                           mid - kResizeGrabHalf, 34.0, 2.0 * kResizeGrabHalf),
                                    QPen(QColor(0x4C, 0x92, 0xFF, 150)),
                                    QBrush(QColor(0x16, 0x1A, 0x22)));
        grip->setZValue(2);
        grip->setAcceptedMouseButtons(Qt::NoButton);
        // Notch tick at the exact center of the band's grab zone.
        scene_.addLine(QLineF(left + kTrackHeaderWidth / 2.0, mid - 3,
                              left + kTrackHeaderWidth / 2.0, mid + 3),
                       QPen(QColor(0x4C, 0x92, 0xFF, 150)))->setZValue(3);
    }
}

bool TimelineWidget::has_timeline_content() const {
    if (!sequence_) return false;
    for (const auto& t : sequence_->video_tracks)
        if (!t.clips.empty()) return true;
    for (const auto& t : sequence_->audio_tracks)
        if (!t.clips.empty()) return true;
    return false;
}

void TimelineWidget::draw_empty_state() {
    const double x = kSceneMargin;
    const double top = tracks_stack_top();
    const double w = std::max(0.0, scene_.sceneRect().right() - x);

    scene_.addRect(QRectF(x, top, w, kEmptyStateHeight),
                   QPen(QColor(0x2A, 0x2F, 0x3C)), QBrush(QColor(0x12, 0x15, 0x1C)));

    // Branded empty state: a Nova-gold film-strip mark above the copy, painted
    // once per empty rebuild (no scene-wide tile loop), sized to the token grid.
    QColor mark = t.accent;
    mark.setAlpha(190);
    add_icon(scene_, QStringLiteral("film-strip"), x + 20, top + 22, mark, 22);

    auto* title = scene_.addText(tr("Drag video or audio clips here to start"));
<<<<<<< Updated upstream
    title->setDefaultTextColor(QColor(0x6B, 0x72, 0x85));
=======
    title->setDefaultTextColor(t.ink);
>>>>>>> Stashed changes
    QFont tf = title->font();
    tf.setPointSizeF(11.5);
    tf.setBold(true);
    title->setFont(tf);
    title->setPos(QPointF(x + 56, top + 22));
    title->setZValue(1);

    auto* hint = scene_.addText(
        tr("Drop clips from the Media Pool into the timeline, or import a file"));
    hint->setDefaultTextColor(QColor(0x4A, 0x50, 0x60));
    QFont sf = hint->font();
    sf.setPointSizeF(9);
    hint->setFont(sf);
    hint->setPos(QPointF(x + 56, top + 48));
    hint->setZValue(1);
}

void TimelineWidget::refresh_transition_bubble(TransitionBubble& b, int64_t duration_frames) {
    // Crisp glass pill drawn ON the clip, spanning the full clip/channel height.
    // Anchored to the cut (spanning both clips) or the clip's IN/OUT edge;
    // no duration text on the canvas — the pill shape carries the info.
    if (!b.pill) return;
    const double w = std::round(std::max(44.0, duration_frames / frames_per_pixel_));
    const double bx = kSceneMargin + kTrackHeaderWidth + b.frame / frames_per_pixel_;
    const double x = std::round(b.cut ? bx - w / 2.0 : (b.in_edge ? bx : bx - w));
    const QRectF r(x, std::round(b.y), w, std::round(b.h));

    b.pill->setPath(rounded_rect_path(r, 6.0));
    b.pill->setPen(QPen(QColor(255, 255, 255, 255), 1.8));
    // Faint frost fill — nearly clear white, just enough tint to blur softly
    // behind the crisp outline while still letting the clip thumbnails read.
    b.pill->setBrush(QColor(255, 255, 255, 28));
    b.hit = r;
}

void TimelineWidget::add_transition_bubbles() {
    // Glass "transition" bubble drawn on the clip(s): one per video track and one
    // per audio track for every edit point that carries a transition. A
    // transition spanning two clips collapses into a single bubble centered on
    // the edit point; single-clip IN/OUT edges hug the clip's start/end.
    if (!sequence_) return;
    const int v_count = static_cast<int>(sequence_->video_tracks.size());

    const auto emit_track = [this, v_count](
                                const std::vector<canvas::core::Track>& tracks, int track_offset,
                                canvas::core::Track::Kind kind) {
        struct Boundary {
            int64_t frame = 0;
            const canvas::core::Clip* left = nullptr;  // clip ending at frame (owns transition_out)
            const canvas::core::Clip* right = nullptr; // clip starting at frame (owns transition_in)
            int64_t dur = 0;
        };
        for (int ti = 0; ti < static_cast<int>(tracks.size()); ++ti) {
            const auto& clips = tracks[ti].clips;
            const int flat = track_offset + ti;

            std::vector<Boundary> bounds;
            for (std::size_t j = 0; j < clips.size(); ++j) {
                const auto& c = clips[j];
                const auto* next =
                    (j + 1 < clips.size() && clips[j + 1].tl_in == c.tl_out)
                        ? &clips[j + 1]
                        : nullptr;
                const auto* prev =
                    (j > 0 && clips[j - 1].tl_out == c.tl_in) ? &clips[j - 1] : nullptr;
                if (c.has_transition_out())
                    bounds.push_back(Boundary{c.tl_out, &c, next, c.transition_out_duration});
                if (c.has_transition_in()) {
                    // Merge with an existing boundary at the same edit point so
                    // a two-sided cut transition yields ONE bubble, not two.
                    bool merged = false;
                    for (auto& bn : bounds) {
                        if (bn.frame == c.tl_in) {
                            bn.right = &c;
                            bn.dur = std::max<int64_t>(bn.dur, c.transition_in_duration);
                            merged = true;
                            break;
                        }
                    }
                    if (!merged)
                        bounds.push_back(Boundary{c.tl_in, prev, &c, c.transition_in_duration});
                }
            }

            std::vector<TransitionBubble> local;
            for (const auto& bn : bounds) {
                if (!bn.left && !bn.right) continue;
                TransitionBubble b;
                b.frame = bn.frame;
                b.y = static_cast<double>(track_top(flat, v_count)) +
                      (kind == canvas::core::Track::Kind::Video ? 2.0 : 3.0);
                b.h = track_height(flat, v_count) - 6.0;
                b.in_edge = (bn.left == nullptr);
                b.cut = (bn.left != nullptr && bn.right != nullptr);
                b.clip_id = bn.left ? bn.left->id : bn.right->id;
                b.b_clip_id = b.cut ? bn.right->id : 0;

                auto* pill = scene_.addPath(QPainterPath());
                pill->setZValue(50);
                pill->setAcceptedMouseButtons(Qt::NoButton);
                // Subtle frosted-glass interior: the faint fill below + a tiny
                // blur soften the pill's edges so it reads as glass, not a
                // hard wireframe.
                auto* frost = new QGraphicsBlurEffect;
                frost->setBlurRadius(1.2);
                pill->setGraphicsEffect(frost);
                b.pill = pill;

                refresh_transition_bubble(b, bn.dur);
                local.push_back(std::move(b));
            }

            // Adjacent bubbles on the same track can never overlap: greedily shrink each
            // bubble's left edge so it clears the previous one, keeping its far
            // edge — and thus its anchor boundary — in place.
            std::sort(local.begin(), local.end(),
                      [](const TransitionBubble& x, const TransitionBubble& y) {
                          return x.hit.left() < y.hit.left();
                      });
            constexpr double kGapPx = 4.0;
            constexpr double kMinBubbleW = 34.0;
            double used_until = -1e9;
            for (auto& b : local) {
                QRectF r = b.hit;
                if (r.left() < used_until) {
                    const double new_left = used_until;
                    r.setLeft(new_left);
                    r.setRight(std::max(new_left + kMinBubbleW, r.right()));
                    b.pill->setPath(rounded_rect_path(r, 6.0));
                    b.hit = r;
                }
                used_until = r.right() + kGapPx;
            }
            for (auto& b : local)
                transition_bubbles_.push_back(std::move(b));
        }
    };

    emit_track(sequence_->video_tracks, 0, canvas::core::Track::Kind::Video);
    emit_track(sequence_->audio_tracks, v_count, canvas::core::Track::Kind::Audio);

    // Restore the selected pill's highlight after a rebuild (re-matched by id).
    if (selected_transition_.valid) {
        for (auto& b : transition_bubbles_) {
            if (!b.pill) continue;
            const bool sel = b.clip_id == selected_transition_.a &&
                             (b.cut ? b.b_clip_id == selected_transition_.b
                                    : b.in_edge == selected_transition_.in_edge);
            if (sel) b.pill->setPen(QPen(QColor(0xFF, 0xD7, 0x4A, 255), 2.0));
        }
    }
}

void TimelineWidget::draw_disabled_marks() {
    // Dim disabled clips (toggled with Ctrl+D) so they read as muted/offline.
    for (const auto& item : clip_items_) {
        if (!item.clip || item.clip->enabled) continue;
        if (item.shell) item.shell->setOpacity(0.35);
        if (item.outline) item.outline->setOpacity(0.35);
        if (item.label_bar) item.label_bar->setOpacity(0.35);
        if (item.text) item.text->setOpacity(0.45);
        for (const auto& cell : item.cells) {
            if (cell.item) cell.item->setOpacity(0.4);
        }
    }
}

void TimelineWidget::draw_playhead() {
    const double x = kSceneMargin + kTrackHeaderWidth + playhead_frame_ / frames_per_pixel_;
    playhead_item_ = scene_.addLine(
        QLineF(x, kSceneMargin, x, scene_.sceneRect().bottom()),
        QPen(QColor(0x4C, 0x92, 0xFF), 1));
    playhead_item_->setZValue(500);
}

void TimelineWidget::update_playhead_position(int64_t frame) {
    if (!playhead_item_) return;
    const double x = kSceneMargin + kTrackHeaderWidth + frame / frames_per_pixel_;
    playhead_item_->setLine(QLineF(x, kSceneMargin, x, scene_.sceneRect().bottom()));
    if (timecode_item_) timecode_item_->setPlainText(timecode(frame, fps_));
    const QRectF visible = mapToScene(viewport()->rect()).boundingRect();
    if (x < visible.left() || x > visible.right()) {
        // Scroll horizontally to follow the playhead, keeping the current
        // vertical position (the scene extends far into the pan room and tail,
        // so vertical centering would fling the tracks out of view).
        centerOn(x, mapToScene(viewport()->rect().center()).y());
    }
    update_minimap_viewport();
}

}  // namespace canvas::gui
