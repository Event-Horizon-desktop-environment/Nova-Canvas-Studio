#pragma once

#include <QGraphicsView>
#include <QGraphicsItem>
#include <QGraphicsScene>
#include <QImage>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
#include <QStyleOptionGraphicsItem>
#include <QObject>
#include <QTimer>
#include <QWheelEvent>
#include <QMouseEvent>
#include <QKeyEvent>
#include <QContextMenuEvent>
#include <QStringList>
#include <QVector>

#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <algorithm>

#include "canvas/core/timeline/model.hpp"
#include "canvas/core/timeline/audio_mix.hpp"
#include "Widgets/timeline_selection.hpp"
#include "Widgets/timeline_drag.hpp"
#include "Widgets/transition_handle_editor.hpp"
#include "features/timeline/timeline_view_options.hpp"

class QGraphicsRectItem;
class QGraphicsLineItem;
class QGraphicsPathItem;
class QGraphicsTextItem;
class QGraphicsSimpleTextItem;
class QGraphicsPixmapItem;
class QGraphicsItemGroup;
class QDragEnterEvent;
class QDragMoveEvent;
class QDragLeaveEvent;
class QDropEvent;
class QMenu;

namespace canvas::gui {

class ThumbnailService;

inline QPainterPath rounded_rect_path(const QRectF& r, qreal radius) {
    QPainterPath p;
    p.addRoundedRect(r, std::min<qreal>(radius, r.height() / 2.0),
                     std::min<qreal>(radius, r.width() / 2.0));
    return p;
}

struct MediaMeta {
    std::string path;
    int64_t total_frames = 0;
    double fps = 0.0;
    bool is_video = false;
};

class ClipClipGroup final : public QGraphicsItem {
public:
    explicit ClipClipGroup(const QRectF& scene_rect) : rect_(scene_rect) {
        setFlag(ItemClipsChildrenToShape);
        setAcceptedMouseButtons(Qt::NoButton);
    }
    [[nodiscard]] QRectF boundingRect() const override { return rect_; }
    void set_shape(const QRectF& scene_rect) { rect_ = scene_rect; prepareGeometryChange(); }
    void paint(QPainter*, const QStyleOptionGraphicsItem*, QWidget*) override {}

private:
    QRectF rect_;
};

inline constexpr int kSceneMargin = 8;
inline constexpr int kFilmstripCellWidth = 24;
inline constexpr int kFilmstripMaxCells = 4096;

inline constexpr std::uint64_t kTimelineThumbNs = 0x1000000000000000ULL;
inline bool is_timeline_thumb_id(std::uint64_t id) noexcept {
    return id == 0 || (id & kTimelineThumbNs) != 0;
}

QPixmap scaled_fit(const QImage& img, int w, int h);

class TimelineWidget final : public QGraphicsView {
    Q_OBJECT

public:
    enum class Tool { Select, Trim, Blade };
    enum class TrimEdge { Head, Tail };

    static constexpr int kTrackHeaderWidth = 96;
    static constexpr int kRulerHeight = 30;
    static constexpr int kMinimapHeight = 24;
    static constexpr int kTrackGap = 4;
    static constexpr double kDefaultTrackHeight = 60.0;
    static constexpr double kMinTrackHeight = 44.0;
    static constexpr double kMaxTrackHeight = 200.0;
    static constexpr double kCollapsedHeightFactor = 0.55;
    static constexpr double kTrackVPad = 14.0;
    static constexpr double kMinTrackVPad = 2.0;
    static constexpr double kMaxTrackVPad = 200.0;
    static constexpr double kEmptyStateHeight = 120.0;
    static constexpr double kResizeGrabHalf = 5.0;
    static constexpr double kSectionDividerHeight = 11.0;
    static constexpr double kVerticalPanTailMin = 240.0;
    static constexpr double kPanDownRoomMin = 240.0;
    static constexpr double kTimecodeBarHeight = 24.0;
    static constexpr double kMinFramesPerPixel = 0.04;
    static constexpr double kMaxFramesPerPixel = 1500.0;
    static constexpr double kDefaultFramesPerPixel = 1.0;
    static constexpr double kZoomMinPercent = 4.0;
    static constexpr double kZoomMaxPercent = 2500.0;
    static constexpr double kClipLabelHeight = 18.0;
    static constexpr double kClipOutlineW = 1.5;
    static constexpr double kClipSelectedOutlineW = 2.2;
    static constexpr double kClipShadowOffset = 2.0;

    explicit TimelineWidget(QWidget* parent = nullptr);
    ~TimelineWidget() override;

    void set_sequence(const canvas::core::Sequence* sequence);
    void set_fps(double fps);
    void set_playhead_position(int64_t frame);
    void set_tool(Tool tool);
    void set_snap_enabled(bool enabled);
    [[nodiscard]] int64_t snap_frame(int64_t frame) const;
    void set_follow_playhead(bool follow) { follow_playhead_ = follow; }
    [[nodiscard]] bool follow_playhead() const { return follow_playhead_; }
    void zoom_fit();
    void zoom_in();
    void zoom_out();
    void set_frames_per_pixel(double fpp);
    void set_zoom_percent(double percent);
    [[nodiscard]] double interactive_floor_percent() const;
    [[nodiscard]] double zoom_percent() const { return kDefaultFramesPerPixel / frames_per_pixel_ * 100.0; }
    void set_thumbnail_service(ThumbnailService* service);
    void set_media_paths(std::unordered_map<canvas::core::MediaId, MediaMeta> paths);
    void set_view_options(const TimelineViewOptions* options);
    void notify_view_options_changed();
    void set_all_video_heights(double height);
    void set_all_audio_heights(double height);
    void set_selection(const std::vector<canvas::core::ClipId>& ids);
    void clear_selection();
    [[nodiscard]] const std::vector<canvas::core::ClipId>& selected_clip_ids() const { return selection_.ids(); }

    [[nodiscard]] double frames_per_pixel() const { return frames_per_pixel_; }

    struct DropLane {
        canvas::core::Track::Kind kind = canvas::core::Track::Kind::Video;
        int index = 0;
    };
    DropLane resolve_drop_lane(double scene_y, canvas::core::Track::Kind media_kind) const;

    [[nodiscard]] bool has_selected_transition() const { return selected_transition_.valid; }
    [[nodiscard]] canvas::core::ClipId selected_transition_a() const { return selected_transition_.a; }
    [[nodiscard]] canvas::core::ClipId selected_transition_b() const { return selected_transition_.b; }
    [[nodiscard]] int desired_timeline_height() const;
    [[nodiscard]] bool has_clips() const { return has_timeline_content(); }
    bool delete_selected_transition();

    struct MovedClip {
        canvas::core::ClipId id = 0;
        int64_t new_tl_in = 0;
        canvas::core::Track::Kind kind = canvas::core::Track::Kind::Video;
        int track_index = 0;
    };

    std::vector<int64_t> collect_snap_targets(
        const std::vector<canvas::core::ClipId>& exclude,
        bool include_playhead) const;

signals:
    void playhead_moved(int64_t frame);
    void playhead_committed(int64_t frame);
    void clip_selected(const canvas::core::Clip* clip);
    void clip_moved(const canvas::core::Clip* clip, int64_t new_tl_in, canvas::core::Track::Kind dst_kind,
                    int dst_track);
    void clips_moved(std::vector<MovedClip> clips);
    void volume_line_preview(float db);
    void volume_line_committed(float db);
    void clip_trimmed(const canvas::core::Clip* clip, TrimEdge edge, int64_t new_frame);
    void blade_requested(const canvas::core::Clip* clip, int64_t frame);
    void range_selected(int64_t in, int64_t out);
    void clips_range_selected(std::vector<canvas::core::ClipId> clip_ids);
    void new_upper_track_requested(canvas::core::ClipId clip_id, int64_t tl_in);
    void media_dropped(int media_id, int64_t frame, double scene_y);
    void media_files_dropped(const QStringList& paths, int64_t frame, double scene_y);
    void title_dropped(const QString& preset_id, int64_t frame, double scene_y);
    void transition_dropped(const QString& transition_id, int64_t frame, double scene_y);
    void unlink_requested(canvas::core::Track::Kind kind, int track_index, canvas::core::ClipId id);
    void link_requested(canvas::core::Track::Kind kind, int track_index, canvas::core::ClipId id);
    void add_track_requested(canvas::core::Track::Kind kind);
    void delete_track_requested(canvas::core::Track::Kind kind, int track_index);
    void track_mute_toggled(canvas::core::Track::Kind kind, int track_index, bool on);
    void track_solo_toggled(canvas::core::Track::Kind kind, int track_index, bool on);
    void track_lock_toggled(canvas::core::Track::Kind kind, int track_index, bool on);
    void track_collapse_toggled(canvas::core::Track::Kind kind, int track_index, bool on);
    void transition_requested(const canvas::core::Clip* clip, canvas::core::TransitionType type,
                              int64_t duration);
    void transition_in_requested(const canvas::core::Clip* clip, canvas::core::TransitionType type,
                                 int64_t duration);
    void clear_transition_requested(const canvas::core::Clip* clip);
    void clear_transition_in_requested(const canvas::core::Clip* clip);
    void clip_color_requested(const canvas::core::Clip* clip, uint8_t color);
    void transition_resized(const canvas::core::Clip* clip, int64_t duration);
    void transition_in_resized(const canvas::core::Clip* clip, int64_t duration);
    void delete_through_edit_requested(const canvas::core::Clip* out_clip);
    void delete_transition_requested(const canvas::core::Clip* a, const canvas::core::Clip* b,
                                     bool in_edge);
    void transition_selected(canvas::core::ClipId a, canvas::core::ClipId b, bool in_edge);
    void transition_selection_cleared();
    void content_height_changed(int height_px);

protected:
    void wheelEvent(QWheelEvent* event) override;
    void contextMenuEvent(QContextMenuEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void keyReleaseEvent(QKeyEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void scrollContentsBy(int dx, int dy) override;
    bool eventFilter(QObject* watched, QEvent* event) override;
    void paintEvent(QPaintEvent* event) override;
    void leaveEvent(QEvent* event) override;
    void dragEnterEvent(QDragEnterEvent* event) override;
    void dragMoveEvent(QDragMoveEvent* event) override;
    void dragLeaveEvent(QDragLeaveEvent* event) override;
    void dropEvent(QDropEvent* event) override;

private slots:
    void on_thumbnail_ready(uint64_t id, const QImage& image);
    void on_waveform_ready(uint64_t id, const QImage& image);

private:
    void rebuild_timeline();
    void relayout_scene();
    void draw_ruler();
    void draw_timecode_bar();
    void draw_tracks();
    bool has_timeline_content() const;
    void draw_empty_state();
    void draw_disabled_marks();
    void draw_playhead();
    void draw_minimap();
    void update_minimap_viewport();
    void update_playhead_position(int64_t frame);
    void request_clip_thumbnails();
    class RulerMarksItem;
    class GridlinesItem;
    double track_top(int track_index, int v_count) const;
    double track_height(int track_index, int v_count) const;
    static int flat_of_screen_row(int s, int v_count) noexcept {
        if (s < v_count) return v_count - 1 - s;
        if (s == v_count) return -1;
        return v_count + (s - v_count - 1);
    }
    double tracks_origin_y() const;
    double tracks_content_height(int v_count, int a_count) const;
    double tracks_stack_top() const;
    double empty_state_top() const;
    double tracks_stack_bottom(int v_count, int a_count) const;
    double edge_y(int edge, int v_count, int a_count) const;
    bool in_section_divider_band(double scene_y, int v_count, int a_count) const;
    int header_resize_target(double scene_y, int v_count, int a_count) const;
    void sync_track_heights();
    void set_track_height(int flat_track, int v_count, double height);
    int track_at_y(double scene_y, int v_count) const;
    int kind_track_index(int flat_track, int v_count) const;
    int64_t frame_at_x(int x) const;
    int64_t blade_cut_frame(int x) const;
    void scrub_to_frame(int64_t frame);
    std::vector<int64_t> snap_targets_;
    void update_blade_preview(int64_t frame);
    void hide_blade_preview();
    void update_snap_indicator(bool snapped, int64_t frame);
    void hide_snap_indicator();
    void apply_selection_highlight();

    using Edge = transition_editor::Edge;
    using CutTarget = transition_editor::CutTarget;
    CutTarget cut_at_scene_pos(const QPointF& scene_pos) const;
    void update_transition_hover(const QPointF& scene_pos);
    void hide_transition_handle();
    void rebuild_transition_handle();
    void press_transition_handle(const QPointF& scene_pos);
    void move_transition_handle(const QPointF& scene_pos);
    void release_transition_handle();
    void engage_transition_drag(const QPointF& scene_pos);
    struct TransitionBubble;
    bool maybe_press_transition_bubble(const QPointF& scene_pos);
    void add_transition_bubbles();
    void select_transition_bubble(std::size_t index);
    void clear_selected_transition();
    void refresh_transition_bubble(TransitionBubble& b, int64_t duration_frames);
    void open_transition_editor_for_clip(const canvas::core::Clip* clip, int64_t frame,
                                         bool in_edge);
    void open_transition_editor_for_cut(const canvas::core::Clip* a,
                                        const canvas::core::Clip* b, int64_t cut_frame);

    static constexpr int kTransitionEdgeNone = transition_editor::kDragEdgeNone;
    static constexpr int kTransitionEdgeLeft = transition_editor::kDragEdgeLeft;
    static constexpr int kTransitionEdgeRight = transition_editor::kDragEdgeRight;
    static constexpr double kCutHoverTolerancePx = 8.0;
    static constexpr int64_t kMinTransitionFrames = transition_editor::kMinTransitionFrames;

    const canvas::core::Sequence* sequence_ = nullptr;
    double fps_ = 30.0;
    double frames_per_pixel_ = 1.0;
    int64_t playhead_frame_ = 0;
    bool follow_playhead_ = true;
    Tool current_tool_ = Tool::Select;
    bool snap_enabled_ = true;
    ThumbnailService* thumbnail_service_ = nullptr;
    uint64_t next_thumb_id_ = kTimelineThumbNs + 1;
    std::unordered_map<canvas::core::MediaId, MediaMeta> media_paths_;

    QGraphicsScene scene_;
    QGraphicsLineItem* playhead_item_ = nullptr;
    QGraphicsRectItem* selection_rect_ = nullptr;
    QGraphicsRectItem* minimap_viewport_ = nullptr;
    QGraphicsRectItem* minimap_background_ = nullptr;
    QGraphicsItemGroup* chrome_ = nullptr;
    bool track_count_changed_ = false;
    bool last_had_content_ = false;
    QGraphicsItemGroup* top_pinned_ = nullptr;
    QGraphicsTextItem* timecode_item_ = nullptr;
    double pan_down_room_ = kPanDownRoomMin;

    struct ClipCell {
        QGraphicsPixmapItem* item = nullptr;
        uint64_t request_id = 0;
    };
    struct ClipItem {
        QGraphicsRectItem* rect = nullptr;
        QGraphicsPathItem* shell = nullptr;
        QGraphicsPathItem* outline = nullptr;
        QGraphicsPathItem* label_bar = nullptr;
        QGraphicsTextItem* text = nullptr;
        std::vector<ClipCell> cells;
        const canvas::core::Clip* clip = nullptr;
        int track_index = 0;
        canvas::core::Track::Kind track_kind = canvas::core::Track::Kind::Video;
        QGraphicsLineItem* volume_line = nullptr;
        double volume_y0 = 0.0;
        double volume_h = 0.0;
    };
    std::vector<ClipItem> clip_items_;

    ClipItem* find_linked_mate(ClipItem* item);
    int64_t snap_trim_edge(int64_t raw_edge, ClipItem* clip);
    void position_clip_at(ClipItem& item, int64_t tl_in);
    void preview_trim_clip(ClipItem& item, TrimEdge edge, int64_t tl_frame);
    bool find_trim_edge(const QPointF& scene_pos, ClipItem*& out_clip, TrimEdge& out_edge);
    bool find_volume_line_hit(const QPointF& scene_pos, ClipItem*& out_clip);
    bool reacquire_dragged_clip(canvas::core::ClipId id, canvas::core::ClipId mate_id,
                                int64_t pointer_frame);
    void shift_transition_bubbles(const std::unordered_set<canvas::core::ClipId>& dragging);
    void set_live_clip_gain(canvas::core::ClipId id, float db);
    void apply_waveform_volume_scale(ClipItem& item, float db);
    [[nodiscard]] const TimelineViewOptions& eff_view_options() const;
    std::vector<canvas::core::ClipId> expand_with_mates(const std::vector<canvas::core::ClipId>& ids);

    int flat_row_at_scene_y(double scene_y) const;
    QGraphicsRectItem* highlight_scene_item(QGraphicsRectItem*& slot);
    void set_rect_highlight(QGraphicsRectItem*& slot, const QRectF& rect,
                            const QBrush& fill, const QPen& pen);
    void set_row_highlight(QGraphicsRectItem*& slot, int flat,
                           const QBrush& fill, const QPen& pen);
    void clear_row_highlight(QGraphicsRectItem*& slot, int& flat);
    void update_hover_row(const QPointF& scene_pos);
    void update_drop_lane(const QPointF& scene_pos);
    void update_drop_preview(const QPoint& widget_pos, int media_id);
    void clear_drop_preview();
    QGraphicsItemGroup* drop_overlay_ = nullptr;
    QGraphicsRectItem* hover_highlight_ = nullptr;
    int hover_flat_ = -1;
    QGraphicsRectItem* drop_lane_highlight_ = nullptr;
    int drop_lane_flat_ = -1;

    struct TrackHeader {
        QGraphicsRectItem* background = nullptr;
        QGraphicsTextItem* label = nullptr;
        QGraphicsSimpleTextItem* badge = nullptr;
        QGraphicsItem* badge_pill = nullptr;
        QGraphicsTextItem* count = nullptr;
        QGraphicsPixmapItem* lock_icon = nullptr;
        QGraphicsPixmapItem* solo_icon = nullptr;
        QGraphicsPixmapItem* mute_icon = nullptr;
        QGraphicsPixmapItem* collapse_icon = nullptr;
        QGraphicsTextItem* channel_badge = nullptr;
    };
    std::vector<TrackHeader> video_track_headers_;
    std::vector<TrackHeader> audio_track_headers_;

    QPointF drag_start_pos_;
    QPointF drag_scene_start_;
    bool marquee_full_height_ = false;
    QPointF drag_press_pos_;
    int64_t drag_start_frame_ = 0;
    double drag_grab_offset_px_ = 0.0;
    ClipItem* dragged_clip_ = nullptr;
    ClipItem* drag_mate_ = nullptr;
    int original_track_index_ = 0;

    std::vector<MovedClip> drag_clip_snapshot_;
    std::vector<int64_t> drag_clip_orig_;
    canvas::core::ClipId drag_primary_id_ = 0;

    ClipItem* find_clip_item(canvas::core::ClipId id);

    bool trimming_ = false;
    ClipItem* trimmed_clip_ = nullptr;
    TrimEdge trim_edge_ = TrimEdge::Head;
    int64_t trim_start_edge_ = 0;
    double trim_grab_offset_px_ = 0.0;

    bool volume_drag_armed_ = false;
    bool volume_dragging_ = false;
    canvas::core::ClipId volume_drag_clip_ = 0;
    float volume_drag_db_ = 0.0f;
    std::vector<canvas::core::ClipId> volume_drag_targets_;

    std::vector<double> video_track_heights_;
    std::vector<double> audio_track_heights_;
    const TimelineViewOptions* view_options_ = nullptr;
    bool resizing_track_ = false;
    int resize_edge_ = -1;
    int resize_total_ = 0;
    double resize_start_y_ = 0.0;
    double resize_above_start_ = 0.0;
    double resize_below_start_ = 0.0;
    bool track_resize_cursor_shown_ = false;
    QTimer* track_resize_timer_ = nullptr;

    bool pan_dragging_ = false;
    double pan_anchor_viewport_y_ = 0.0;
    int pan_start_scroll_ = 0;

    double track_v_pad_top_ = 15.0;
    double track_v_pad_bottom_ = kTrackVPad;
    bool is_dragging_ = false;
    bool promote_latched_ = false;

    timeline_drag::DragController drag_ctrl_;
    bool is_selecting_range_ = false;
    int64_t range_start_frame_ = 0;
    bool is_scrubbing_ = false;
    QGraphicsLineItem* blade_preview_item_ = nullptr;
    QGraphicsLineItem* snap_indicator_item_ = nullptr;
    std::vector<canvas::core::ClipId> selected_clip_ids_;

    timeline_selection::SelectionState selection_;

    transition_editor::Editor transition_editor_;
    QGraphicsPathItem* transition_overlay_ = nullptr;
    QGraphicsItem* transition_icon_ = nullptr;
    QList<QGraphicsItem*> transition_items_;

    struct TransitionBubble {
        canvas::core::ClipId clip_id = 0;
        canvas::core::ClipId b_clip_id = 0;
        int64_t frame = 0;
        int64_t dur = 0;
        bool in_edge = false;
        bool cut = false;
        double y = 0.0;
        double h = 0.0;
        QRectF hit;
        QGraphicsPathItem* pill = nullptr;
    };
    std::vector<TransitionBubble> transition_bubbles_;
    bool transition_press_armed_ = false;
    QPointF transition_press_pos_;
    const canvas::core::Clip* transition_press_a_ = nullptr;
    const canvas::core::Clip* transition_press_b_ = nullptr;
    int64_t transition_press_frame_ = 0;
    bool transition_press_in_edge_ = false;

    struct SelectedTransition {
        canvas::core::ClipId a = 0;
        canvas::core::ClipId b = 0;
        bool in_edge = false;
        bool valid = false;
    };
    SelectedTransition selected_transition_;
};

}