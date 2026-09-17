#include "Widgets/timeline_widget.hpp"
#include "Logging.hpp"
#include "UX/theme.hpp"
#include "timeline_volume_line.hpp"

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
#include <QGraphicsPathItem>
#include <QGraphicsSimpleTextItem>
#include <QPainterPath>
#include <QPainter>
#include <QPixmap>
#include <QSvgRenderer>
#include <QTransform>

#include <algorithm>
#include <cmath>
#include <chrono>

namespace canvas::gui {

void TimelineWidget::rebuild_timeline() {
    const ThemeTokens& t = tokens();
    const auto rebuild_t0 = std::chrono::steady_clock::now();
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
    top_pinned_ = nullptr;
    chrome_ = nullptr;
    blade_preview_item_ = nullptr;
    snap_indicator_item_ = nullptr;
    hover_highlight_ = nullptr;
    hover_flat_ = -1;
    drop_lane_highlight_ = nullptr;
    drop_lane_flat_ = -1;
    drop_overlay_ = nullptr;

    transition_items_.clear();
    transition_bubbles_.clear();
    transition_overlay_ = nullptr;
    transition_icon_ = nullptr;
    transition_editor_.close();

    sync_track_heights();

    const double dur_frames = sequence_ ? static_cast<double>(sequence_->duration_frames()) : 0.0;
    const double content_end = kSceneMargin + kTrackHeaderWidth + dur_frames / frames_per_pixel_;
    const double visible_end = kSceneMargin + viewport()->width();
    const double width = std::max(content_end, visible_end) - kSceneMargin + 400.0;
    const int v_count = sequence_ ? static_cast<int>(sequence_->video_tracks.size()) : 1;
    const int a_count = sequence_ ? static_cast<int>(sequence_->audio_tracks.size()) : 0;
    const bool has_content = has_timeline_content();
    const double content_height = has_content
        ? tracks_content_height(v_count, a_count)
        : (empty_state_top() - static_cast<double>(kSceneMargin)) + kEmptyStateHeight
              + track_v_pad_bottom_;
    const double vp_h = static_cast<double>(viewport()->height());
    pan_down_room_ = kPanDownRoomMin;
    const double height = std::max(content_height, vp_h) + kVerticalPanTailMin;

    scene_.setSceneRect(kSceneMargin, kSceneMargin, width, height);

    chrome_ = new QGraphicsItemGroup();
    chrome_->setAcceptedMouseButtons(Qt::NoButton);
    chrome_->setHandlesChildEvents(false);
    chrome_->setZValue(-1.0);
    scene_.addItem(chrome_);
    QGraphicsRectItem* bg = scene_.addRect(
        QRectF(kSceneMargin, kSceneMargin, width, height), QPen(Qt::NoPen), QBrush(t.surface));
    bg->setAcceptedMouseButtons(Qt::NoButton);
    chrome_->addToGroup(bg);

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
    const bool content_transitioned = has_content != last_had_content_;
    const bool structural = track_count_changed_ || content_transitioned;
    last_had_content_ = has_content;
    const int park = has_content
        ? static_cast<int>(lround(pan_down_room_ + track_v_pad_top_))
        : 0;
    const int reach = std::max(park, static_cast<int>(lround(kSceneMargin + content_height - vp_h)));
    verticalScrollBar()->setRange(0, std::max(reach, 0));
    if (structural || follow_playhead_) verticalScrollBar()->setValue(park);
    if (!has_content) verticalScrollBar()->setValue(0);
    if (top_pinned_) top_pinned_->setPos(0.0, static_cast<double>(verticalScrollBar()->value()));
    if (structural) {
        track_count_changed_ = false;
        if (has_content)
            emit content_height_changed(desired_timeline_height());
    }
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
        qDebug() << "[ui:timeline] rebuild ms_avg=" << QString::number(s_rb_ms / s_rb_n, 'f', 2)
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

void TimelineWidget::relayout_scene() {
    const ThemeTokens& t = tokens();
    if (!sequence_) return;
    if (!has_timeline_content()) {
        rebuild_timeline();
        return;
    }
    const auto rl_t0 = std::chrono::steady_clock::now();
    const double dur_frames = static_cast<double>(sequence_->duration_frames());
    const double content_end = kSceneMargin + kTrackHeaderWidth + dur_frames / frames_per_pixel_;
    const double visible_end = kSceneMargin + viewport()->width();
    const double width = std::max(content_end, visible_end) - kSceneMargin + 400.0;
    const int v_count = sequence_ ? static_cast<int>(sequence_->video_tracks.size()) : 1;
    const int a_count = sequence_ ? static_cast<int>(sequence_->audio_tracks.size()) : 0;
    const bool has_content = has_timeline_content();
    const double content_height = has_content
        ? tracks_content_height(v_count, a_count)
        : (empty_state_top() - static_cast<double>(kSceneMargin)) + kEmptyStateHeight
              + track_v_pad_bottom_;
    const double vp_h = static_cast<double>(viewport()->height());
    pan_down_room_ = kPanDownRoomMin;
    const double height = std::max(content_height, vp_h) + kVerticalPanTailMin;

    scene_.setSceneRect(kSceneMargin, kSceneMargin, width, height);

    if (chrome_) {
        scene_.removeItem(chrome_);
        delete chrome_;
        chrome_ = nullptr;
    }
    if (top_pinned_) {
        scene_.removeItem(top_pinned_);
        delete top_pinned_;
        top_pinned_ = nullptr;
    }
    timecode_item_ = nullptr;
    minimap_background_ = nullptr;
    minimap_viewport_ = nullptr;
    if (selection_rect_) {
        scene_.removeItem(selection_rect_);
        delete selection_rect_;
        selection_rect_ = nullptr;
    }

    chrome_ = new QGraphicsItemGroup();
    chrome_->setAcceptedMouseButtons(Qt::NoButton);
    chrome_->setHandlesChildEvents(false);
    chrome_->setZValue(-1.0);
    scene_.addItem(chrome_);
    QGraphicsRectItem* bg = scene_.addRect(
        QRectF(kSceneMargin, kSceneMargin, width, height), QPen(Qt::NoPen), QBrush(t.surface));
    bg->setAcceptedMouseButtons(Qt::NoButton);
    chrome_->addToGroup(bg);

    top_pinned_ = new QGraphicsItemGroup();
    top_pinned_->setAcceptedMouseButtons(Qt::NoButton);
    top_pinned_->setHandlesChildEvents(false);
    top_pinned_->setZValue(400);
    scene_.addItem(top_pinned_);

    draw_timecode_bar();
    draw_minimap();
    draw_ruler();

    update_playhead_position(playhead_frame_);
    const int park = has_content
        ? static_cast<int>(lround(pan_down_room_ + track_v_pad_top_))
        : 0;
    const int reach = std::max(park, static_cast<int>(lround(kSceneMargin + content_height - vp_h)));
    verticalScrollBar()->setRange(0, std::max(reach, 0));
    if (follow_playhead_) verticalScrollBar()->setValue(park);
    if (!has_content) verticalScrollBar()->setValue(0);
    if (top_pinned_) top_pinned_->setPos(0.0, static_cast<double>(verticalScrollBar()->value()));

    const double rl_ms = std::chrono::duration<double, std::milli>(
                             std::chrono::steady_clock::now() - rl_t0).count();
    static auto s_rl_at = std::chrono::steady_clock::now();
    static int s_rl_n = 0;
    static double s_rl_ms = 0.0, s_max_ms = 0.0;
    ++s_rl_n;
    s_rl_ms += rl_ms;
    s_max_ms = std::max(s_max_ms, rl_ms);
    const auto rl_now = std::chrono::steady_clock::now();
    if (s_rl_n == 1 || rl_now - s_rl_at >= std::chrono::seconds(1)) {
        s_rl_at = rl_now;
        qDebug() << "[ui:timeline] relayout ms_avg=" << QString::number(s_rl_ms / s_rl_n, 'f', 2)
                   << "ms_last=" << QString::number(rl_ms, 'f', 2)
                   << "ms_max=" << QString::number(s_max_ms, 'f', 2)
                   << "relayouts/s=" << s_rl_n
                   << "items=" << scene_.items().size()
                   << "scene_w=" << QString::number(scene_.sceneRect().width(), 'f', 0)
                   << "scene_h=" << QString::number(scene_.sceneRect().height(), 'f', 0);
        s_rl_n = 0;
        s_rl_ms = 0.0;
        s_max_ms = 0.0;
    }
}

void TimelineWidget::draw_timecode_bar() {
    const ThemeTokens& t = tokens();
    auto* bg = scene_.addRect(
        QRectF(kSceneMargin, 0.0, scene_.sceneRect().width(), kTimecodeBarHeight),
        QPen(t.border), QBrush(t.surface_raised));
    bg->setAcceptedMouseButtons(Qt::NoButton);
    top_pinned_->addToGroup(bg);

    timecode_item_ = scene_.addText(timecode(playhead_frame_, fps_));
    timecode_item_->setAcceptedMouseButtons(Qt::NoButton);
    timecode_item_->setDefaultTextColor(t.ink);
    QFont f = timecode_item_->font();
    f.setFamily(QStringLiteral("Monospace"));
    f.setBold(true);
    f.setPointSizeF(9.5);
    timecode_item_->setFont(f);
    timecode_item_->setPos(kSceneMargin + 6.0, 4.5);
    top_pinned_->addToGroup(timecode_item_);
}

void TimelineWidget::draw_minimap() {
    const ThemeTokens& t = tokens();
    const double left = kSceneMargin;
    const double top = kTimecodeBarHeight + kSceneMargin;
    const double width = scene_.sceneRect().width();
    minimap_background_ = scene_.addRect(
        QRectF(left, top, width, kMinimapHeight),
        QPen(t.border), QBrush(t.surface_raised));
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
                                        QBrush(t.accent));
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
        QPen(t.playhead), QBrush(t.playhead_soft));
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
    minimap_viewport_->setRect(QRectF(std::min(vp_x, max_x), top,
                                      std::min(vp_w, std::max(4.0, max_x - std::min(vp_x, max_x))),
                                      kMinimapHeight));
}

namespace {

void compute_ruler_steps(const double fpp, double& step, int& major_mod) {
    step = 1.0;
    while (step / fpp < 8.0) step *= 2.0;
    while (step / fpp > 18.0) step /= 2.0;
    major_mod = std::max(1, static_cast<int>(std::ceil(90.0 / (step / fpp))));
}

}

class TimelineWidget::RulerMarksItem : public QGraphicsItem {
public:
    explicit RulerMarksItem(TimelineWidget* owner) : owner_(owner) {
        setAcceptedMouseButtons(Qt::NoButton);
        setHandlesChildEvents(false);
    }

    QRectF boundingRect() const override {
        const double top = kMinimapHeight + kSceneMargin
                           + kTimecodeBarHeight;
        return QRectF(kSceneMargin, top,
                      owner_->scene_.sceneRect().width() - kSceneMargin,
                      kRulerHeight);
    }

    void paint(QPainter* painter, const QStyleOptionGraphicsItem* option, QWidget*) override {
        if (owner_->frames_per_pixel_ <= 0.0) return;
        double step = 0.0;
        int major_mod = 1;
        compute_ruler_steps(owner_->frames_per_pixel_, step, major_mod);
        const double top = kMinimapHeight + kSceneMargin
                           + kTimecodeBarHeight;
        const double x0 = kSceneMargin + kTrackHeaderWidth;
        const double fpp = owner_->frames_per_pixel_;
        const double left = std::max(option->exposedRect.left(), x0);
        const double right = std::min(option->exposedRect.right(),
                                      owner_->scene_.sceneRect().right());
        if (right <= left) return;

        const ThemeTokens& t = tokens();
        QColor major_color = t.ink_muted; major_color.setAlpha(170);
        const QPen major_pen(major_color);
        QColor minor_color = t.ink_muted; minor_color.setAlpha(90);
        const QPen minor_pen(minor_color);
        QFont label_font;
        label_font.setPointSizeF(8);

        const double i_max = (right - x0) * fpp / step;
        int64_t i = std::max<int64_t>(
            0, static_cast<int64_t>(std::ceil((left - x0) * fpp / step)));
        for (; i <= static_cast<int64_t>(i_max) + 1; ++i) {
            const double x = x0 + i * step / fpp;
            if (x > right) break;
            const bool major = (i % major_mod) == 0;
            if (major) {
                painter->setPen(major_pen);
                painter->drawLine(QLineF(x, top, x, top + (i == 0 ? 16.0 : 12.0)));
                painter->setPen(t.ink_muted);
                painter->setFont(label_font);
                const QString label = timecode(static_cast<int64_t>(i * step), owner_->fps_);
                const double gap_top = top + 16.0;
                const double gap_bot = top + kRulerHeight - 8.0;
                const double gap_cy = (gap_top + gap_bot) * 0.5;
                const double label_h = QFontMetricsF(label_font).height();
                painter->drawText(QPointF(x + 2, gap_cy - label_h * 0.5), label);
            } else {
                painter->setPen(minor_pen);
                painter->drawLine(QLineF(x, top + kRulerHeight - 8, x,
                                         top + kRulerHeight));
            }
        }
    }

private:
    TimelineWidget* owner_;
};

class TimelineWidget::GridlinesItem : public QGraphicsItem {
public:
    explicit GridlinesItem(TimelineWidget* owner) : owner_(owner) {
        setAcceptedMouseButtons(Qt::NoButton);
        setHandlesChildEvents(false);
    }

    QRectF boundingRect() const override { return owner_->scene_.sceneRect(); }

    void paint(QPainter* painter, const QStyleOptionGraphicsItem* option, QWidget*) override {
        if (owner_->frames_per_pixel_ <= 0.0) return;
        double step = 0.0;
        int major_mod = 1;
        compute_ruler_steps(owner_->frames_per_pixel_, step, major_mod);
        const double top = kMinimapHeight + kSceneMargin
                           + kTimecodeBarHeight;
        const double bottom = owner_->scene_.sceneRect().bottom();
        const double x0 = kSceneMargin + kTrackHeaderWidth;
        const double fpp = owner_->frames_per_pixel_;
        const double left = std::max(option->exposedRect.left(), x0);
        const double right = std::min(option->exposedRect.right(),
                                      owner_->scene_.sceneRect().right());
        if (right <= left) return;

        const ThemeTokens& t = tokens();
        QColor grid_color = t.border; grid_color.setAlpha(110);
        const QPen grid_pen(grid_color);
        painter->setPen(grid_pen);
        const double i_max = (right - x0) * fpp / step;
        int64_t i = std::max<int64_t>(
            0, static_cast<int64_t>(std::ceil((left - x0) * fpp / step)));
        for (; i <= static_cast<int64_t>(i_max) + 1; ++i) {
            const double x = x0 + i * step / fpp;
            if (x > right) break;
            if (i % major_mod) continue;
            painter->drawLine(QLineF(x, top, x, bottom));
        }
    }

private:
    TimelineWidget* owner_;
};

void TimelineWidget::draw_ruler() {
    const ThemeTokens& t = tokens();
    const double top = kMinimapHeight + kSceneMargin + kTimecodeBarHeight;
    auto* bg = scene_.addRect(
        QRectF(kSceneMargin, top, scene_.sceneRect().width(), kRulerHeight),
        QPen(t.border), QBrush(t.surface_raised));
    if (top_pinned_) {
        bg->setAcceptedMouseButtons(Qt::NoButton);
        top_pinned_->addToGroup(bg);
    }

    if (top_pinned_) {
        auto* marks = new RulerMarksItem(this);
        top_pinned_->addToGroup(marks);
    }
    if (chrome_) {
        auto* grids = new GridlinesItem(this);
        chrome_->addToGroup(grids);
    }

    if (sequence_) {
        const QPen bk_pen(QColor(0xFF, 0xC1, 0x07), 1);
        const QBrush bk_brush(QColor(0xFF, 0xC1, 0x07));
        for (const auto& b : sequence_->bookmarks) {
            const double bx = kSceneMargin + kTrackHeaderWidth + b.frame / frames_per_pixel_;
            auto* bk = scene_.addRect(QRectF(bx - 3, top + kRulerHeight - 14, 6, 6),
                                      bk_pen, bk_brush);
            if (top_pinned_) {
                bk->setAcceptedMouseButtons(Qt::NoButton);
                top_pinned_->addToGroup(bk);
            }
        }
    }

    auto* divider = scene_.addLine(
        QLineF(kSceneMargin + kTrackHeaderWidth, top, kSceneMargin + kTrackHeaderWidth, scene_.sceneRect().bottom()),
        QPen(t.border));
    if (chrome_) chrome_->addToGroup(divider);
}

namespace {

QColor blend_colors(const QColor& over, const QColor& base, double t) {
    const auto l = [t](int oc, int bc) {
        return static_cast<int>(std::lround(bc + (oc - bc) * t));
    };
    return QColor(l(over.red(), base.red()), l(over.green(), base.green()),
                  l(over.blue(), base.blue()), base.alpha());
}

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

struct TrackBadge {
    QGraphicsPathItem* pill = nullptr;
    QGraphicsSimpleTextItem* text = nullptr;
};

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
    const QRectF br = text_item->boundingRect();
    text_item->setPos(r.center() - br.center());
    text_item->setZValue(3);
    return {pill_item, text_item};
}

}

void TimelineWidget::draw_tracks() {
    const ThemeTokens& t = tokens();
    const int v_count = sequence_ ? static_cast<int>(sequence_->video_tracks.size()) : 1;
    const int a_count = sequence_ ? static_cast<int>(sequence_->audio_tracks.size()) : 0;

    const double left = kSceneMargin;
    const QColor kIconColor = t.ink_muted;
    const QColor kIconColorDim = t.ink_faint;
    const QColor kIconAccent = t.accent_text;

    if (!has_timeline_content()) {
        draw_empty_state();
        return;
    }

    const double h_top = tracks_stack_top();
    const double h_bot = tracks_stack_bottom(v_count, a_count);
    scene_.addRect(QRectF(left, h_top, kTrackHeaderWidth, h_bot - h_top),
                   QPen(Qt::NoPen), QBrush(t.surface_low))->setZValue(0);
    scene_.addLine(QLineF(left + kTrackHeaderWidth, h_top,
                          left + kTrackHeaderWidth, h_bot),
                   QPen(t.border))->setZValue(0);

    for (int i = 0; i < v_count; ++i) {
        const double y = track_top(i, v_count);
        const double th = track_height(i, v_count);
        QString tname = QString::fromStdString(sequence_->video_tracks[i].name);

        const bool full = th >= 52.0;

        const QString badge_text = tname.isEmpty() ? QStringLiteral("V%1").arg(i + 1) : tname;
        const double badge_top = full ? y + 8.0 : y + (th - 15.0) / 2.0;
        TrackBadge tb = add_badge(scene_, badge_text, QPointF(left + 28, badge_top),
                                  t.surface_highest, t.accent);
        auto* badge_pill = tb.pill;
        auto* badge = tb.text;

        const bool vcollapsed = sequence_->video_tracks[i].collapsed;
        QGraphicsPixmapItem* collapse_icon =
            add_icon(scene_, vcollapsed ? QStringLiteral("chevron_right")
                                        : QStringLiteral("chevron_down"),
                     left + 8, y + (th - 13.0) / 2.0, kIconColor);

        scene_.addRect(QRectF(left, y + 1, 4, th - 2), QPen(Qt::NoPen),
                       QBrush(t.accent))->setZValue(1);

        QGraphicsPixmapItem* lock_icon = nullptr;
        const bool vlocked = sequence_->video_tracks[i].locked;
        if (!vcollapsed && full) {
            add_icon(scene_, QStringLiteral("reorder"), left + kTrackHeaderWidth - 44, y + 10, kIconColorDim, 13);
            lock_icon = add_icon(scene_, vlocked ? QStringLiteral("lock") : QStringLiteral("unlock"),
                                 left + kTrackHeaderWidth - 26, y + 9,
                                 vlocked ? kIconAccent : kIconColor);
        } else if (!vcollapsed) {
            const double ic_y = y + (th - 13.0) / 2.0;
            add_icon(scene_, QStringLiteral("reorder"), left + kTrackHeaderWidth - 53, ic_y, kIconColorDim, 13);
            lock_icon = add_icon(scene_, vlocked ? QStringLiteral("lock") : QStringLiteral("unlock"),
                                 left + kTrackHeaderWidth - 37, ic_y,
                                 vlocked ? kIconAccent : kIconColor);
        }

        QGraphicsTextItem* label = nullptr;
        if (full && th >= 56.0) {
            label = scene_.addText(QStringLiteral("Video %1").arg(i + 1));
            label->setDefaultTextColor(t.ink_muted);
            QFont lf = label->font();
            lf.setPointSizeF(9);
            lf.setBold(true);
            label->setFont(lf);
            label->setPos(QPointF(left + 28, y + 28));
            label->setZValue(1);
        }

        QGraphicsTextItem* count = nullptr;
        if (full) {
            count = scene_.addText(QStringLiteral("%1 Clips").arg(sequence_->video_tracks[i].clips.size()));
            count->setDefaultTextColor(t.ink_faint);
            QFont cf = count->font();
            cf.setPointSizeF(8);
            count->setFont(cf);
            count->setPos(QPointF(left + 28, std::min(y + 44.0, y + th - 14.0)));
            count->setZValue(1);
        }
        TrackHeader header;
        header.background = nullptr;
        header.label = label;
        header.badge = badge;
        header.badge_pill = badge_pill;
        header.count = count;
        header.lock_icon = lock_icon;
        header.collapse_icon = collapse_icon;
        video_track_headers_.push_back(header);

        const canvas::core::Track* track = &sequence_->video_tracks[i];
        const int track_index = i;
        for (const auto& clip : track->clips) {
            const double cx = left + kTrackHeaderWidth + clip.tl_in / frames_per_pixel_;
            const double cw = clip.duration() / frames_per_pixel_;
            const double cy = y + 2;
            const double ch = th - 6;
            const TimelineViewOptions& vopts = eff_view_options();
            const double label_h = std::min(kClipLabelHeight, ch * 0.35);
            const double body_h = ch - label_h;
            auto* rect = scene_.addRect(
                QRectF(0, 0, cw, ch), QPen(Qt::NoPen), QBrush(Qt::transparent));
            rect->setPos(QPointF(cx, cy));
            rect->setAcceptedMouseButtons(Qt::NoButton);
            const QColor cc_v = clip_color_for(clip.clip_color);
            const QColor shell_v =
                cc_v.isValid() ? blend_colors(cc_v, t.clip_video, 0.65) : t.clip_video;
            auto* shadow = scene_.addPath(
                rounded_rect_path(QRectF(0, kClipShadowOffset, cw, ch), 6),
                QPen(Qt::NoPen), QBrush(t.clip_shadow));
            shadow->setPos(QPointF(cx, cy));
            shadow->setAcceptedMouseButtons(Qt::NoButton);
            auto* shell = scene_.addPath(
                rounded_rect_path(QRectF(0, 0, cw, ch), 6),
                QPen(Qt::NoPen), QBrush(shell_v));
            shell->setPos(QPointF(cx, cy));
            shell->setAcceptedMouseButtons(Qt::NoButton);
            auto* outline = scene_.addPath(
                rounded_rect_path(QRectF(kClipOutlineW / 2.0, kClipOutlineW / 2.0,
                                         cw - kClipOutlineW, ch - kClipOutlineW), 6),
                QPen(t.clip_border_video, kClipOutlineW), QBrush(Qt::NoBrush));
            outline->setPos(QPointF(cx, cy));
            outline->setZValue(2.0);
            outline->setAcceptedMouseButtons(Qt::NoButton);

            QGraphicsPathItem* label_bar = nullptr;
            if (vopts.show_clip_names) {
                label_bar = scene_.addPath(
                    rounded_rect_path(QRectF(0, body_h, cw, label_h), 6),
                    QPen(Qt::NoPen),
                    QBrush(cc_v.isValid() ? shell_v.darker(135) : t.clip_label));
                label_bar->setPos(QPointF(cx, cy));
                label_bar->setZValue(0);
            }

            QGraphicsTextItem* text = nullptr;
            if (vopts.show_clip_names && cw > 60.0) {
                text = scene_.addText(QString::fromStdString(clip.name));
                text->setDefaultTextColor(t.ink);
                QFont tf = text->font();
                tf.setPointSizeF(7.5);
                text->setFont(tf);
                text->setPos(QPointF(cx + 4, cy + body_h + 1));
                text->setZValue(2);
                text->setAcceptedMouseButtons(Qt::NoButton);
            }

            QGraphicsTextItem* duration = nullptr;
            if (vopts.show_clip_durations && cw > 84.0) {
                duration = scene_.addText(timecode(clip.duration(), fps_));
                duration->setDefaultTextColor(t.ink_muted);
                QFont df = duration->font();
                df.setPointSizeF(7.0);
                duration->setFont(df);
                duration->setPos(QPointF(cx + cw - 6 - duration->boundingRect().width(), cy + 3));
                duration->setZValue(2);
                duration->setAcceptedMouseButtons(Qt::NoButton);
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

            auto* clip_group = new ClipClipGroup(QRectF(cx, cy, cw, ch));
            scene_.addItem(clip_group);
            for (QGraphicsItem* child : std::initializer_list<QGraphicsItem*>{shadow, rect, shell, outline, label_bar, text, duration})
                if (child) child->setParentItem(clip_group);

            if (clip.has_transition_out() &&
                !canvas::core::is_audio_transition(clip.transition_out) && cw > 40.0) {
                add_icon(scene_, QStringLiteral("transition_out"), cx + cw - 16, cy + 3,
                         QColor(0xF2, 0xA9, 0x4A), 12);
            }

            if (clip.has_transition_in() &&
                !canvas::core::is_audio_transition(clip.transition_in) && cw > 40.0) {
                add_icon(scene_, QStringLiteral("transition_in"), cx + 4, cy + 3,
                         QColor(0x4A, 0xA9, 0xF2), 12);
            }

            int num_cells = 0;
            if (vopts.thumbnails == ThumbnailMode::Filmstrip)
                num_cells = std::min(std::max(1, static_cast<int>(cw / kFilmstripCellWidth)), kFilmstripMaxCells);
            else if (vopts.thumbnails == ThumbnailMode::SingleFrame)
                num_cells = 1;
            const double cell_w = num_cells > 0 ? cw / num_cells : 0.0;
            for (int c = 0; c < num_cells; ++c) {
                auto* cell = scene_.addPixmap(QPixmap());
                cell->setPos(cx + c * cell_w, cy + 2);
                cell->setZValue(1);
                cell->setAcceptedMouseButtons(Qt::NoButton);
                item.cells.push_back(ClipCell{cell, 0});
            }
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
        const bool is_music_role = i >= 2;
        const QColor role_color = is_music_role ? QColor(0xC9, 0x86, 0x3A) : QColor(0x2E, 0x8F, 0xC0);
        const bool full = th >= 52.0;

        const double badge_top = full ? y + 8.0 : y + (th - 15.0) / 2.0;
        TrackBadge tb = add_badge(scene_, badge_text, QPointF(left + 28, badge_top),
                                  t.surface_highest,
                                  is_music_role ? t.accent_text : t.playhead);
        auto* badge_pill = tb.pill;
        auto* badge = tb.text;

        const auto& hdr_track = sequence_->audio_tracks[i];
        const bool acollapsed = hdr_track.collapsed;
        QGraphicsPixmapItem* collapse_icon =
            add_icon(scene_, acollapsed ? QStringLiteral("chevron_right")
                                        : QStringLiteral("chevron_down"),
                     left + 8, y + (th - 13.0) / 2.0, kIconColor);

        scene_.addRect(QRectF(left, y + 1, 4, th - 2), QPen(Qt::NoPen),
                       QBrush(role_color))->setZValue(1);

        QGraphicsPixmapItem* lock_icon = nullptr;
        QGraphicsPixmapItem* solo_icon = nullptr;
        QGraphicsPixmapItem* mute_icon = nullptr;
        const bool alocked = hdr_track.locked;
        const bool asolo = hdr_track.solo;
        const bool amuted = hdr_track.muted;
        if (full && !acollapsed) {
            lock_icon = add_icon(scene_, alocked ? QStringLiteral("lock") : QStringLiteral("unlock"),
                                 left + kTrackHeaderWidth - 26, y + 9,
                                 alocked ? kIconAccent : kIconColor);
            solo_icon = add_icon(scene_, QStringLiteral("solo"), left + kTrackHeaderWidth - 44, y + 20,
                                 asolo ? QColor(0xF2, 0xA9, 0x3C) : kIconColor);
            mute_icon = add_icon(scene_, amuted ? QStringLiteral("mute") : QStringLiteral("volume"),
                                 left + kTrackHeaderWidth - 26, y + 20,
                                 amuted ? QColor(0xEF, 0x44, 0x44) : kIconColor);
        } else if (!acollapsed) {
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

        QGraphicsTextItem* label = nullptr;
        if (full && th >= 56.0) {
            label = scene_.addText(QStringLiteral("Audio %1").arg(i + 1));
            label->setDefaultTextColor(t.ink_muted);
            QFont lf = label->font();
            lf.setPointSizeF(9);
            lf.setBold(true);
            label->setFont(lf);
            label->setPos(QPointF(left + 28, y + 28));
            label->setZValue(1);
        }

        QGraphicsTextItem* count = nullptr;
        if (full) {
            count = scene_.addText(QStringLiteral("%1 Clips").arg(sequence_->audio_tracks[i].clips.size()));
            count->setDefaultTextColor(t.ink_faint);
            QFont cf = count->font();
            cf.setPointSizeF(8);
            count->setFont(cf);
            count->setPos(QPointF(left + 28, std::min(y + 44.0, y + th - 14.0)));
            count->setZValue(1);
        }

        QGraphicsTextItem* channel_badge = nullptr;
        if (full) {
            channel_badge = scene_.addText(QStringLiteral("2.0"));
            channel_badge->setDefaultTextColor(t.ink_faint);
            QFont chf = channel_badge->font();
            chf.setPointSizeF(7);
            channel_badge->setFont(chf);
            channel_badge->setPos(QPointF(left + kTrackHeaderWidth - 24, y + th - 16));
            channel_badge->setZValue(1);
        }

        TrackHeader header;
        header.background = nullptr;
        header.label = label;
        header.badge = badge;
        header.badge_pill = badge_pill;
        header.count = count;
        header.lock_icon = lock_icon;
        header.solo_icon = solo_icon;
        header.mute_icon = mute_icon;
        header.collapse_icon = collapse_icon;
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
            const TimelineViewOptions& vopts = eff_view_options();

            auto* rect = scene_.addRect(
                QRectF(0, 0, cw, ch), QPen(Qt::NoPen), QBrush(Qt::transparent));
            rect->setPos(QPointF(cx, cy));
            rect->setAcceptedMouseButtons(Qt::NoButton);
            const QColor cc_a = clip_color_for(clip.clip_color);
            const QColor shell_a =
                cc_a.isValid() ? blend_colors(cc_a, t.clip_audio, 0.65) : t.clip_audio;
            auto* shadow = scene_.addPath(
                rounded_rect_path(QRectF(0, kClipShadowOffset, cw, ch), 6),
                QPen(Qt::NoPen), QBrush(t.clip_shadow));
            shadow->setPos(QPointF(cx, cy));
            shadow->setAcceptedMouseButtons(Qt::NoButton);
            auto* shell = scene_.addPath(
                rounded_rect_path(QRectF(0, 0, cw, ch), 6),
                QPen(Qt::NoPen), QBrush(shell_a));
            shell->setPos(QPointF(cx, cy));
            shell->setAcceptedMouseButtons(Qt::NoButton);
            auto* outline = scene_.addPath(
                rounded_rect_path(QRectF(kClipOutlineW / 2.0, kClipOutlineW / 2.0,
                                         cw - kClipOutlineW, ch - kClipOutlineW), 6),
                QPen(t.clip_border_audio, kClipOutlineW), QBrush(Qt::NoBrush));
            outline->setPos(QPointF(cx, cy));
            outline->setZValue(2.0);
            outline->setAcceptedMouseButtons(Qt::NoButton);

            QGraphicsTextItem* text = nullptr;
            if (vopts.show_clip_names && cw > 60.0) {
                text = scene_.addText(QString::fromStdString(clip.name));
                text->setDefaultTextColor(t.ink);
                QFont tf = text->font();
                tf.setPointSizeF(7.5);
                text->setFont(tf);
                text->setPos(QPointF(cx + 5, cy + body_h + 1));
                text->setZValue(2);
                text->setAcceptedMouseButtons(Qt::NoButton);
            }
            QGraphicsTextItem* duration = nullptr;
            if (vopts.show_clip_durations && cw > 84.0) {
                duration = scene_.addText(timecode(clip.duration(), fps_));
                duration->setDefaultTextColor(t.ink_muted);
                QFont df = duration->font();
                df.setPointSizeF(7.0);
                duration->setFont(df);
                duration->setPos(QPointF(cx + cw - 6 - duration->boundingRect().width(), cy + 3));
                duration->setZValue(2);
                duration->setAcceptedMouseButtons(Qt::NoButton);
            }
            QGraphicsPixmapItem* wf = nullptr;
            if (vopts.show_waveforms) {
                wf = scene_.addPixmap(QPixmap());
                wf->setPos(cx, cy + 2);
                wf->setZValue(1);
                wf->setAcceptedMouseButtons(Qt::NoButton);
            }

            const double vol_y0 = cy + 2.0;
            const double vol_h = std::max(1.0, ch - 4.0);
            const double vol_y = vol_y0 + timeline_volume_line::volume_line_y(clip.volume_db, vol_h);
            auto* vol = scene_.addLine(QLineF(cx + 2.5, vol_y, cx + cw - 2.5, vol_y),
                                       QPen(t.playhead, 2.0));
            vol->setZValue(1.5);
            vol->setAcceptedMouseButtons(Qt::NoButton);

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
            if (wf) item.cells.push_back(ClipCell{wf, 0});
            item.clip = &clip;
            item.track_index = v_count + track_index;
            item.track_kind = canvas::core::Track::Kind::Audio;
            item.volume_line = vol;
            item.volume_y0 = vol_y0;
            item.volume_h = vol_h;

            auto* clip_group = new ClipClipGroup(QRectF(cx, cy, cw, ch));
            scene_.addItem(clip_group);
            for (QGraphicsItem* child : std::initializer_list<QGraphicsItem*>{shadow, rect, shell, outline, text, duration, vol})
                if (child) child->setParentItem(clip_group);
            for (const auto& cell : item.cells)
                if (cell.item) cell.item->setParentItem(clip_group);
            clip_items_.push_back(std::move(item));
        }
    }

    const int total_t = v_count + a_count;
    const double right_edge = scene_.sceneRect().right();
    const QPen row_pen(t.border_soft);
    for (int t = 0; t < total_t; ++t) {
        if (t == v_count - 1 && a_count > 0) continue;
        const double bottom = track_top(t, v_count) + track_height(t, v_count);
        scene_.addLine(QLineF(left, bottom, right_edge, bottom), row_pen);
    }

    if (v_count > 0 && a_count > 0) {
        const double band_top = edge_y(v_count, v_count, a_count);
        const double band_bot = edge_y(v_count + 1, v_count, a_count);
        const double band_w = std::max(0.0, right_edge - left);
        scene_.addRect(QRectF(left, band_top, band_w, band_bot - band_top),
                       QPen(Qt::NoPen), QBrush(t.surface_low))->setZValue(0);
        const double mid = (band_top + band_bot) / 2.0;
        scene_.addLine(QLineF(left, mid - 1, right_edge, mid - 1),
                       QPen(t.accent, 2.0))->setZValue(1);
        QColor grip_pen_c = t.accent; grip_pen_c.setAlpha(150);
        auto* grip = scene_.addRect(QRectF(left + (kTrackHeaderWidth - 34.0) / 2.0,
                                           mid - kResizeGrabHalf, 34.0, 2.0 * kResizeGrabHalf),
                                    QPen(grip_pen_c),
                                    QBrush(t.surface_low));
        grip->setZValue(2);
        grip->setAcceptedMouseButtons(Qt::NoButton);
        scene_.addLine(QLineF(left + kTrackHeaderWidth / 2.0, mid - 3,
                              left + kTrackHeaderWidth / 2.0, mid + 3),
                       QPen(grip_pen_c))->setZValue(3);
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
    const ThemeTokens& t = tokens();
    const double x = kSceneMargin;
    const double top = empty_state_top();
    const double w = std::max(0.0, scene_.sceneRect().right() - x);

    scene_.addRect(QRectF(x, top, w, kEmptyStateHeight),
                   QPen(t.border), QBrush(t.surface_low));

    QColor mark = t.accent;
    mark.setAlpha(190);
    add_icon(scene_, QStringLiteral("film-strip"), x + 20, top + 22, mark, 22);

    auto* title = scene_.addText(tr("Drag video or audio clips here to start"));
    title->setDefaultTextColor(t.ink);
    QFont tf = title->font();
    tf.setPointSizeF(11.5);
    tf.setBold(true);
    title->setFont(tf);
    title->setPos(QPointF(x + 56, top + 22));
    title->setZValue(1);

    auto* hint = scene_.addText(
        tr("Drop clips from the Media Pool into the timeline, or import a file"));
    hint->setDefaultTextColor(t.ink_faint);
    QFont sf = hint->font();
    sf.setPointSizeF(9);
    hint->setFont(sf);
    hint->setPos(QPointF(x + 56, top + 48));
    hint->setZValue(1);
}

void TimelineWidget::refresh_transition_bubble(TransitionBubble& b, int64_t duration_frames) {
    if (!b.pill) return;
    const ThemeTokens& t = tokens();
    constexpr double kMinPillW = 10.0;
    const double w = std::round(std::max(kMinPillW, duration_frames / frames_per_pixel_));
    const double bx = kSceneMargin + kTrackHeaderWidth + b.frame / frames_per_pixel_;
    const double x = std::round(b.cut ? bx - w / 2.0 : (b.in_edge ? bx : bx - w));
    const QRectF r(x, std::round(b.y), w, std::round(b.h));

    b.pill->setPath(rounded_rect_path(r, 6.0));
    b.pill->setPen(QPen(t.ink, 1.8));
    b.pill->setBrush(t.state_hover);
    b.hit = r;
}

void TimelineWidget::add_transition_bubbles() {
    if (!sequence_) return;
    const int v_count = static_cast<int>(sequence_->video_tracks.size());

    const auto emit_track = [this, v_count](
                                const std::vector<canvas::core::Track>& tracks, int track_offset,
                                canvas::core::Track::Kind kind) {
        struct Boundary {
            int64_t frame = 0;
            const canvas::core::Clip* left = nullptr;
            const canvas::core::Clip* right = nullptr;
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
                b.dur = bn.dur;

                auto* pill = scene_.addPath(QPainterPath());
                pill->setZValue(50);
                pill->setAcceptedMouseButtons(Qt::NoButton);
                auto* frost = new QGraphicsBlurEffect;
                frost->setBlurRadius(1.2);
                pill->setGraphicsEffect(frost);
                b.pill = pill;

                refresh_transition_bubble(b, bn.dur);
                local.push_back(std::move(b));
            }

            std::sort(local.begin(), local.end(),
                      [](const TransitionBubble& x, const TransitionBubble& y) {
                          return x.hit.left() < y.hit.left();
                      });
            constexpr double kGapPx = 4.0;
            constexpr double kMinBubbleW = 10.0;
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

void TimelineWidget::shift_transition_bubbles(
    const std::unordered_set<canvas::core::ClipId>& dragging) {
    const double left_edge = kSceneMargin + kTrackHeaderWidth;
    for (auto& b : transition_bubbles_) {
        canvas::core::ClipId anchor = b.clip_id;
        bool from_left = true;
        if (!dragging.count(anchor)) {
            if (b.cut && b.b_clip_id != 0 && dragging.count(b.b_clip_id)) {
                anchor = b.b_clip_id;
                from_left = false;
            } else {
                continue;
            }
        }
        const ClipItem* item = find_clip_item(anchor);
        if (!item || !item->rect) continue;
        const double x = item->rect->scenePos().x();
        const double w = item->rect->rect().width();
        const int64_t cur_in =
            static_cast<int64_t>(std::llround((x - left_edge) * frames_per_pixel_));
        const int64_t span = w > 0.0
            ? static_cast<int64_t>(std::llround(w * frames_per_pixel_))
            : (item->clip ? item->clip->duration() : 0);
        if (from_left && !b.in_edge && !b.cut) {
            b.frame = cur_in + span;
        } else if (from_left && b.cut) {
            b.frame = cur_in + span;
        } else {
            b.frame = cur_in;
        }
        refresh_transition_bubble(b, b.dur);
    }
}

void TimelineWidget::set_live_clip_gain(canvas::core::ClipId id, float db) {
    ClipItem* item = find_clip_item(id);
    if (!item || !item->volume_line || !item->rect) return;
    const QLineF cur = item->volume_line->line();
    const double y = item->volume_y0 + timeline_volume_line::volume_line_y(db, item->volume_h);
    item->volume_line->setLine(QLineF(cur.x1(), y, cur.x2(), y));
    apply_waveform_volume_scale(*item, db);
}

void TimelineWidget::apply_waveform_volume_scale(ClipItem& item, float db) {
    if (item.track_kind != canvas::core::Track::Kind::Audio || item.cells.empty()) return;
    QGraphicsPixmapItem* wf = item.cells[0].item;
    if (!wf || wf->pixmap().isNull()) return;
    const double h = wf->pixmap().height();
    if (h <= 1.0) return;
    const double s = timeline_volume_line::volume_waveform_scale(db);
    QTransform t;
    t.translate(0.0, h / 2.0);
    t.scale(1.0, s);
    t.translate(0.0, -h / 2.0);
    wf->setTransform(t);
}

void TimelineWidget::draw_disabled_marks() {
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
    const ThemeTokens& t = tokens();
    const double x = kSceneMargin + kTrackHeaderWidth + playhead_frame_ / frames_per_pixel_;
    playhead_item_ = scene_.addLine(
        QLineF(x, kSceneMargin, x, scene_.sceneRect().bottom()),
        QPen(t.playhead, 1));
    playhead_item_->setZValue(500);
}

void TimelineWidget::update_playhead_position(int64_t frame) {
    if (!playhead_item_) return;
    const double x = kSceneMargin + kTrackHeaderWidth + frame / frames_per_pixel_;
    playhead_item_->setLine(QLineF(x, kSceneMargin, x, scene_.sceneRect().bottom()));
    if (timecode_item_) timecode_item_->setPlainText(timecode(frame, fps_));
    const QRectF visible = mapToScene(viewport()->rect()).boundingRect();
    if (follow_playhead_ && (x < visible.left() || x > visible.right())) {
        centerOn(x, mapToScene(viewport()->rect().center()).y());
    }
    update_minimap_viewport();
}

}
