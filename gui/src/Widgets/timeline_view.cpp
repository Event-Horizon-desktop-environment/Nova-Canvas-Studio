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
    chrome_ = nullptr;
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

    // Unpinned scene chrome that scales with the scene rect (full-scene
    // background, ruler gridlines, header divider) lives in a group at a low z
    // so clip/track items always paint above it. relayout_scene() re-renders
    // this group on resize without touching the clip items.
    chrome_ = new QGraphicsItemGroup();
    chrome_->setAcceptedMouseButtons(Qt::NoButton);
    chrome_->setHandlesChildEvents(false);
    chrome_->setZValue(-1.0);
    scene_.addItem(chrome_);
    QGraphicsRectItem* bg = scene_.addRect(
        QRectF(kSceneMargin, kSceneMargin, width, height), QPen(Qt::NoPen), QBrush(t.surface));
    bg->setAcceptedMouseButtons(Qt::NoButton);
    chrome_->addToGroup(bg);

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
    // through the whole room below it. When the channel stack is taller than the
    // viewport the range extends so every channel stays reachable. A rebuild
    // must NOT stomp the user's scroll: re-park only on a structural change
    // (channel count / empty->content transition) or while still following the
    // playhead; plain edits/zoom rebuilds otherwise preserve the scroll.
    const bool content_transitioned = has_content != last_had_content_;
    const bool structural = track_count_changed_ || content_transitioned;
    last_had_content_ = has_content;
    const int park = static_cast<int>(lround(pan_down_room_ + track_v_pad_top_));
    const int reach = std::max(park, static_cast<int>(lround(kSceneMargin + content_height - vp_h)));
    verticalScrollBar()->setRange(0, std::max(reach, 0));
    if (structural || follow_playhead_) verticalScrollBar()->setValue(park);
    if (top_pinned_) top_pinned_->setPos(0.0, static_cast<double>(verticalScrollBar()->value()));
    // Refit the dock only on structural changes: a channel-count change or the
    // empty->content transition. Track-height drags (rebuild per pixel with the
    // same channel count / content state) must not resize the dock underneath
    // the user's drag.
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
    // View-only relayout on a pure viewport resize. Unlike rebuild_timeline(),
    // this recomputes the scene rect and re-renders ONLY the view chrome (scene
    // background, ruler/minimap/timecode strip, gridlines, divider, playhead).
    // Clip items, track headers, filmstrip/waveform pixmaps and their pending
    // thumbnail requests all survive: their geometry is anchored to timeline
    // coords (frame + track height), never to the viewport size, so a height
    // change with a clip-heavy timeline is now O(viewport chrome) instead of a
    // full scene_ teardown + thumbnail re-request.
    const ThemeTokens& t = tokens();
    if (!sequence_) return;
    // The empty-state placeholder is a scene-level rect (not in chrome_), so a
    // width resize wouldn't re-draw it; an empty timeline is trivially cheap to
    // fully rebuild anyway.
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
        : (tracks_stack_top() - static_cast<double>(kSceneMargin)) + kEmptyStateHeight
              + track_v_pad_bottom_;
    const double vp_h = static_cast<double>(viewport()->height());
    pan_down_room_ = kPanDownRoomMin;
    const double height = std::max(content_height, vp_h) + kVerticalPanTailMin;

    scene_.setSceneRect(kSceneMargin, kSceneMargin, width, height);

    // Tear down ONLY the chrome (background + pinned strip + gridlines), leaving
    // every clip/track/filmstrip item and its decoded pixmaps in the scene.
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

    // Playhead survived the chrome teardown; re-anchor it to the new scene rect
    // and re-park the channels under the ruler strip (reach extends to keep
    // every channel scrollable when the stack is taller than the viewport).
    // The re-park is follow-only: a pure resize must not stomp the vertical
    // scroll of a user who has scrolled away.
    update_playhead_position(playhead_frame_);
    const int park = static_cast<int>(lround(pan_down_room_ + track_v_pad_top_));
    const int reach = std::max(park, static_cast<int>(lround(kSceneMargin + content_height - vp_h)));
    verticalScrollBar()->setRange(0, std::max(reach, 0));
    if (follow_playhead_) verticalScrollBar()->setValue(park);
    if (top_pinned_) top_pinned_->setPos(0.0, static_cast<double>(verticalScrollBar()->value()));

    // Per-relayout cost of the chrome-only resize fast path. This is the number
    // to watch when a dock/viewer resize feels sticky: if this stays low but
    // [ui:timeline] resize is high, the fat is elsewhere in resizeEvent (or a
    // full rebuild is sneaking back in).
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
    // Pinned "current time" bar: a persistent playhead readout at the very top of
    // the timeline (above the minimap), refreshed whenever the playhead moves.
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
    const double x = std::min(vp_x + vp_w, left + strip_w);
    minimap_viewport_->setRect(QRectF(std::min(vp_x, max_x), top,
                                      std::min(vp_w, std::max(4.0, max_x - std::min(vp_x, max_x))),
                                      kMinimapHeight));
}

namespace {

// Shared ruler tick geometry: minor ticks land every `step` frames (~8-18px on
// screen at the current zoom); every `major_mod`-th tick is a major tick with a
// full-height gridline + timecode label (~90px label spacing so they never
// collide). Both procedural painters below use this so gridlines and ruler
// marks can never drift from each other.
void compute_ruler_steps(const double fpp, double& step, int& major_mod) {
    step = 1.0;
    while (step / fpp < 8.0) step *= 2.0;
    while (step / fpp > 18.0) step /= 2.0;
    major_mod = std::max(1, static_cast<int>(std::ceil(90.0 / (step / fpp))));
}

}  // namespace

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
        // Draw ONLY the ticks/labels inside the exposed (visible) x-range. At
        // 100% zoom a ~2k-px viewport exposes ~125 ticks — not the ~10k ticks
        // that a 125k-px scene would otherwise allocate as scene items.
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

        // First tick index whose x >= exposed left.
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
                // Vertically center the label in the gap between the longest
                // major tick (top+16) and the minor ticks (top+22): the clean
                // band with no tick marks, matching where the numbers used to
                // read against the old ruler padding.
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

    // Procedural chrome: one item paints the pinned ruler ticks + timecode
    // labels, one paints the full-height major gridlines behind the tracks.
    // Both repaint only the exposed x-range (see compute_ruler_steps), so these
    // two items replace the ~10k+ QGraphicsLineItem/QGraphicsTextItem the old
    // per-tick loop allocated across the whole scene width on every rebuild AND
    // every relayout — the root of "resizing is slow" on wide timelines.
    if (top_pinned_) {
        auto* marks = new RulerMarksItem(this);
        top_pinned_->addToGroup(marks);
    }
    if (chrome_) {
        auto* grids = new GridlinesItem(this);
        chrome_->addToGroup(grids);
    }

    // Bookmarks (Mark > Add Marker, M): small amber diamonds in the ruler.
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

    // Continuous vertical divider between the header column and the scrollable
    // track area, running through every track (unpinned, in the chrome group).
    auto* divider = scene_.addLine(
        QLineF(kSceneMargin + kTrackHeaderWidth, top, kSceneMargin + kTrackHeaderWidth, scene_.sceneRect().bottom()),
        QPen(t.border));
    if (chrome_) chrome_->addToGroup(divider);
}

namespace {

// Blends `over` onto `base` by `t` (0 = base, 1 = over). Used to tone the clip
// swatches a little toward the dark body fills so a coloured clip reads as
// tinted rather than neon.
QColor blend_colors(const QColor& over, const QColor& base, double t) {
    const auto l = [t](int oc, int bc) {
        return static_cast<int>(std::lround(bc + (oc - bc) * t));
    };
    return QColor(l(over.red(), base.red()), l(over.green(), base.green()),
                  l(over.blue(), base.blue()), base.alpha());
}

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
    const ThemeTokens& t = tokens();
    const int v_count = sequence_ ? static_cast<int>(sequence_->video_tracks.size()) : 1;
    const int a_count = sequence_ ? static_cast<int>(sequence_->audio_tracks.size()) : 0;

    const double left = kSceneMargin;
    const QColor kIconColor = t.ink_muted;        // muted ink for icons
    const QColor kIconColorDim = t.ink_faint;     // faint ink for dim icons
    const QColor kIconAccent = t.accent_text;     // active-state ink (lock/mute/solo)

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
                   QPen(Qt::NoPen), QBrush(t.surface_low))->setZValue(0);
    scene_.addLine(QLineF(left + kTrackHeaderWidth, h_top,
                          left + kTrackHeaderWidth, h_bot),
                   QPen(t.border))->setZValue(0);

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
        TrackBadge tb = add_badge(scene_, badge_text, QPointF(left + 8, badge_top),
                                  t.surface_highest, t.accent);
        auto* badge_pill = tb.pill;
        auto* badge = tb.text;

        // Thin full-height accent bar on the far-left edge (per-track-type code).
        scene_.addRect(QRectF(left, y + 1, 4, th - 2), QPen(Qt::NoPen),
                       QBrush(t.accent))->setZValue(1);

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
            label->setDefaultTextColor(t.ink_muted);
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
            count->setDefaultTextColor(t.ink_faint);
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

            // Semi-rounded label bar: flat muted blue strip with the clip filename,
            // hugging the bottom of the clip. Toned toward black a bit more than
            // the shell (135 = ~0.74x) so the filename stays legible.
            auto* label_bar = scene_.addPath(
                rounded_rect_path(QRectF(0, body_h, cw, label_h), 6),
                QPen(Qt::NoPen),
                QBrush(cc_v.isValid() ? shell_v.darker(135) : t.clip_label));
            label_bar->setPos(QPointF(cx, cy));
            label_bar->setZValue(0);

            QGraphicsTextItem* text = nullptr;
            if (cw > 60.0) {
                text = scene_.addText(QString::fromStdString(clip.name));
                text->setDefaultTextColor(t.ink);
                QFont tf = text->font();
                tf.setPointSizeF(7.5);
                text->setFont(tf);
                text->setPos(QPointF(cx + 4, cy + body_h + 1));
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
            label->setDefaultTextColor(t.ink_muted);
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
            count->setDefaultTextColor(t.ink_faint);
            QFont cf = count->font();
            cf.setPointSizeF(8);
            count->setFont(cf);
            count->setPos(QPointF(left + 8, std::min(y + 44.0, y + th - 14.0)));
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
            if (cw > 60.0) {
                text = scene_.addText(QString::fromStdString(clip.name));
                text->setDefaultTextColor(t.ink);
                QFont tf = text->font();
                tf.setPointSizeF(7.5);
                text->setFont(tf);
                text->setPos(QPointF(cx + 5, cy + body_h + 1));
                text->setZValue(2);
                text->setAcceptedMouseButtons(Qt::NoButton);
            }
            auto* wf = scene_.addPixmap(QPixmap());
            // The wave pixmap must sit EXACTLY on the clip body's left edge (cx).
            // frame_at_x() has no x-inset: scene_x = kSceneMargin + kTrackHeaderWidth
            // + frame/fpp. Any horizontal inset here shifts the drawn spectrum
            // relative to the razor grid -> blade aims land N frames off (the
            // 2026-09-08 wrong-cut bug). The vertical +2 inset is fine (y has no
            // frame meaning).
            wf->setPos(cx, cy + 2);
            wf->setZValue(1);
            wf->setAcceptedMouseButtons(Qt::NoButton);

            // Persistent audio volume line (Phase 5): a luminous hairline at the
            // clip's current gain. It doubles as a drag handle (see the volume
            // drag session in timeline_interaction) and a readout. The travel
            // box spans the FULL clip body (2px inset like the waveform top),
            // so the lowest volume lands on the clip's very bottom edge — not
            // stopping above the label strip where the name is drawn.
            const double vol_y0 = cy + 2.0;  // matches the waveform pixmap's box
            const double vol_h = std::max(1.0, ch - 4.0);
            const double vol_y = vol_y0 + timeline_volume_line::volume_line_y(clip.volume_db, vol_h);
            auto* vol = scene_.addLine(QLineF(cx + 2.5, vol_y, cx + cw - 2.5, vol_y),
                                       QPen(t.playhead, 2.0));
            vol->setZValue(1.5);
            vol->setAcceptedMouseButtons(Qt::NoButton);

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
            item.volume_line = vol;
            item.volume_y0 = vol_y0;
            item.volume_h = vol_h;

            // Clip every child to the clip's own rect (see video branch).
            auto* clip_group = new ClipClipGroup(QRectF(cx, cy, cw, ch));
            scene_.addItem(clip_group);
            for (QGraphicsItem* child : std::initializer_list<QGraphicsItem*>{shadow, rect, shell, outline, text, vol})
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
    const QPen row_pen(t.border_soft);
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
        // Notch tick at the exact center of the band's grab zone.
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
    const double top = tracks_stack_top();
    const double w = std::max(0.0, scene_.sceneRect().right() - x);

    scene_.addRect(QRectF(x, top, w, kEmptyStateHeight),
                   QPen(t.border), QBrush(t.surface_low));

    // Branded empty state: a Nova-gold film-strip mark above the copy, painted
    // once per empty rebuild (no scene-wide tile loop), sized to the token grid.
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
    // Crisp glass pill drawn ON the clip, spanning the full clip/channel height.
    // Anchored to the cut (spanning both clips) or the clip's IN/OUT edge;
    // no duration text on the canvas — the pill shape carries the info.
    if (!b.pill) return;
    const ThemeTokens& t = tokens();
    // Width tracks the transition's real span at the current zoom (dur/fpp) so
    // the pill grows and shrinks with zoom-in/out; only a thin clickable floor
    // keeps it grabbable at extreme zoom-out instead of a fat invariant bubble.
    constexpr double kMinPillW = 10.0;
    const double w = std::round(std::max(kMinPillW, duration_frames / frames_per_pixel_));
    const double bx = kSceneMargin + kTrackHeaderWidth + b.frame / frames_per_pixel_;
    const double x = std::round(b.cut ? bx - w / 2.0 : (b.in_edge ? bx : bx - w));
    const QRectF r(x, std::round(b.y), w, std::round(b.h));

    b.pill->setPath(rounded_rect_path(r, 6.0));
    b.pill->setPen(QPen(t.ink, 1.8));
    // Faint frost fill — nearly clear, just enough tint to soften the pill's
    // edge while still letting the clip thumbnails read behind it.
    b.pill->setBrush(t.state_hover);
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
                b.dur = bn.dur;

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
        // Right edge in frames from the LIVE rect (a drag/trim preview moves the
        // rect before the model commits, so the model's duration would be stale).
        const double w = item->rect->rect().width();
        const int64_t cur_in =
            static_cast<int64_t>(std::llround((x - left_edge) * frames_per_pixel_));
        const int64_t span = w > 0.0
            ? static_cast<int64_t>(std::llround(w * frames_per_pixel_))
            : (item->clip ? item->clip->duration() : 0);
        if (from_left && !b.in_edge && !b.cut) {
            // Single out-edge: pill hugs the clip's CURRENT right edge.
            b.frame = cur_in + span;
        } else if (from_left && b.cut) {
            // Cut carried by its left clip: edit point = its current tl_out.
            b.frame = cur_in + span;
        } else {
            // In-edge single, or cut carried by its right clip: the anchor's
            // current tl_in IS the edit point.
            b.frame = cur_in;
        }
        refresh_transition_bubble(b, b.dur);
    }
}

void TimelineWidget::set_live_clip_gain(canvas::core::ClipId id, float db) {
    // Live volume-line move: re-anchor the clip's gain line at `db` within its
    // stored waveform box. The model is untouched — the commit arrives from
    // volume_line_committed on release. Scene-space: the line is a child of the
    // (0,0)-positioned clip group, so its line coordinates ARE scene coords.
    ClipItem* item = find_clip_item(id);
    if (!item || !item->volume_line || !item->rect) return;
    const QLineF cur = item->volume_line->line();
    const double y = item->volume_y0 + timeline_volume_line::volume_line_y(db, item->volume_h);
    item->volume_line->setLine(QLineF(cur.x1(), y, cur.x2(), y));
    // Keep the spectrum in step with the line while dragging (floored scale, so
    // the readout can never collapse into a line).
    apply_waveform_volume_scale(*item, db);
}

void TimelineWidget::apply_waveform_volume_scale(ClipItem& item, float db) {
    // Scales the audio clip's waveform pixmap vertically, about the box's
    // vertical center, by the floored gain law (timeline_volume_line). The
    // spectrum therefore tracks the volume up/down without flattening — and the
    // item's filled height stays inside the waveform box.
    if (item.track_kind != canvas::core::Track::Kind::Audio || item.cells.empty()) return;
    QGraphicsPixmapItem* wf = item.cells[0].item;
    if (!wf || wf->pixmap().isNull()) return;
    const double h = wf->pixmap().height();
    if (h <= 1.0) return;
    const double s = timeline_volume_line::volume_waveform_scale(db);
    // Scale about the vertical center so the bars stay anchored to the 0 dB
    // midline of the box (louder = taller above AND below it, quieter = shorter
    // both ways — the same meaning the gain line itself carries).
    QTransform t;
    t.translate(0.0, h / 2.0);
    t.scale(1.0, s);
    t.translate(0.0, -h / 2.0);
    wf->setTransform(t);
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
    // Follow only while the user hasn't scrolled away: manual scroll/zoom turns
    // follow_playhead_ off, and an explicit playhead jump re-enables it.
    if (follow_playhead_ && (x < visible.left() || x > visible.right())) {
        // Scroll horizontally to follow the playhead, keeping the current
        // vertical position (the scene extends far into the pan room and tail,
        // so vertical centering would fling the tracks out of view).
        centerOn(x, mapToScene(viewport()->rect().center()).y());
    }
    update_minimap_viewport();
}

}  // namespace canvas::gui
