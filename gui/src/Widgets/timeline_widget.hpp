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
#include <QWheelEvent>
#include <QMouseEvent>
#include <QKeyEvent>
#include <QContextMenuEvent>
#include <QStringList>
#include <QVector>

#include <string>
#include <unordered_map>
#include <vector>
#include <algorithm>

#include "canvas/core/timeline/model.hpp"
#include "Widgets/timeline_selection.hpp"
#include "Widgets/timeline_drag.hpp"

class QGraphicsRectItem;
class QGraphicsLineItem;
class QGraphicsPathItem;
class QGraphicsTextItem;
class QGraphicsSimpleTextItem;
class QGraphicsPixmapItem;
class QGraphicsItemGroup;
class QDragEnterEvent;
class QDragMoveEvent;
class QDropEvent;
class QMenu;

namespace canvas::gui {

class ThumbnailService;

// A rounded rectangle path used for the semi-rounded clip blocks.
inline QPainterPath rounded_rect_path(const QRectF& r, qreal radius) {
    QPainterPath p;
    p.addRoundedRect(r, std::min<qreal>(radius, r.height() / 2.0),
                     std::min<qreal>(radius, r.width() / 2.0));
    return p;
}

// Per-media metadata the timeline carries alongside each path so it can map a
// clip's src_in..src_out window to a fraction of the media for the waveform.
struct MediaMeta {
    std::string path;
    int64_t total_frames = 0;
};

// Transparent clip container used purely as a paint clip: children (filmstrip
// cells, waveform, outline, label) are painted strictly inside the clip's own
// rect, so an abutting clip's content can never visually overlap its neighbour.
class ClipClipGroup final : public QGraphicsItem {
public:
    explicit ClipClipGroup(const QRectF& scene_rect) : rect_(scene_rect) {
        setFlag(ItemClipsChildrenToShape);
        setAcceptedMouseButtons(Qt::NoButton);
    }
    [[nodiscard]] QRectF boundingRect() const override { return rect_; }
    void paint(QPainter*, const QStyleOptionGraphicsItem*, QWidget*) override {}

private:
    QRectF rect_;
};

inline constexpr int kSceneMargin = 8;
// The filmstrip pitch. A denser pitch means more cells = more distinct frame
// samples per clip, and it moves continuously with zoom: every zoom notch
// changes the clip's pixel width enough to add/remove cells and thus forces
// NEW thumbnail frame positions instead of re-serving the previous pass.
inline constexpr int kFilmstripCellWidth = 24;

QPixmap scaled_fill(const QImage& img, int w, int h);

class TimelineWidget final : public QGraphicsView {
    Q_OBJECT

public:
    enum class Tool { Select, Trim, Blade };

    static constexpr int kTrackHeaderWidth = 96;
    static constexpr int kRulerHeight = 30;
    static constexpr int kMinimapHeight = 24;
    static constexpr int kTrackGap = 4;
    // Per-track heights: each row keeps its own size (Resolve-style). The user
    // drags a header edge to grow/shrink the row; kTrackVPad is the empty strip
    // above the topmost video track and below the bottom-most audio track that
    // leaves room for starting a drag-select marquee.
    static constexpr double kDefaultTrackHeight = 60.0;
    static constexpr double kMinTrackHeight = 44.0;
    static constexpr double kMaxTrackHeight = 200.0;
    // Default empty strip above the top-most video row and below the bottom-most
    // audio row (the stack paddings). The paddings are themselves resizeable via
    // the stack-edge resize dividers and keep room to start a drag-select marquee
    // even when every track is near its minimum height.
    static constexpr double kTrackVPad = 14.0;
    static constexpr double kMinTrackVPad = 2.0;
    static constexpr double kMaxTrackVPad = 200.0;
    // Height of the placeholder panel shown when the timeline has no clips yet
    // (track rows stay fully hidden until a video/audio clip is dragged in).
    static constexpr double kEmptyStateHeight = 120.0;
    // Half-height (px) of the grab zone around any row-resize divider and the
    // Video/Audio section divider, either side of the divider line itself.
    static constexpr double kResizeGrabHalf = 5.0;
    // Height (px) of the fixed Video/Audio section divider band that separates
    // V1 from A1. It lives in the place a regular row gap used to sit, is owned
    // by neither section (V1 and A1 cannot resize into it) and its interior is
    // the grab-and-scroll pan handle for the whole timeline. Its top edge is
    // V1's resize boundary and its bottom edge is A1's resize boundary; the two
    // ±kResizeGrabHalf zones never meet (band > 2*half), leaving a clean pan grip
    // in the middle.
    static constexpr double kSectionDividerHeight = 22.0;
    // Vertical panning is grab-and-follow (like dragging a filmstrip): pulling
    // the divider UP seats the channels under the ruler strip and STOPS (that
    // parked scroll value is the range maximum -- the limit). Pulling it DOWN
    // sinks them with the cursor into the deep dark room below, so the down
    // direction reads as an endless void. The scene is also infinite wide.
    static constexpr double kVerticalPanTailMin = 1000.0;
    // Minimum depth of the "sink room" below the parked channel position; the
    // pan-down void the divider pulls the rows into. Scales with the viewport
    // height (2x) so the descent feels unbounded.
    static constexpr double kPanDownRoomMin = 600.0;
    // Pinned "current time" bar above the minimap/ruler: a live readout of the
    // playhead position (HH:MM:SS:FF) glued to the top of the timeline. The
    // whole pinned strip (bar + minimap + ruler) rides in top_pinned_.
    static constexpr double kTimecodeBarHeight = 24.0;
    static constexpr double kMinFramesPerPixel = 0.5;
    static constexpr double kMaxFramesPerPixel = 300.0;
    // Interactive zoom is expressed as a percentage of the 100% baseline, which
    // is one frame per pixel (kDefaultFramesPerPixel). So 200% = 2 px/frame
    // (deep enough to reveal per-frame thumbnails and a detailed spectrum) and
    // 50% = 1 frame per 2 px. zoom_fit() is exempt so long sequences still fit.
    static constexpr double kDefaultFramesPerPixel = 1.0;
    static constexpr double kZoomMinPercent = 50.0;
    static constexpr double kZoomMaxPercent = 200.0;
    // Height of the flat label bar (filename strip) at the bottom of each clip.
    static constexpr double kClipLabelHeight = 18.0;
    static constexpr double kClipOutlineW = 1.5;   // clip-bound stroke; inset by pen/2 so it never overhangs
    static constexpr double kClipSelectedOutlineW = 2.2;

    explicit TimelineWidget(QWidget* parent = nullptr);

    void set_sequence(const canvas::core::Sequence* sequence);
    void set_fps(double fps);
    void set_playhead_position(int64_t frame);
    void set_tool(Tool tool);
    void set_snap_enabled(bool enabled);
    void zoom_fit();
    void zoom_in();
    void zoom_out();
    void set_frames_per_pixel(double fpp);
    void set_zoom_percent(double percent);
    [[nodiscard]] double zoom_percent() const { return kDefaultFramesPerPixel / frames_per_pixel_ * 100.0; }
    void set_thumbnail_service(ThumbnailService* service);
    void set_media_paths(std::unordered_map<canvas::core::MediaId, MediaMeta> paths);
    // Highlight a set of selected clip ids (paints their shells blue).
    void set_selection(const std::vector<canvas::core::ClipId>& ids);
    void clear_selection();
    // The ids currently selected in the timeline (visual selection).
    [[nodiscard]] const std::vector<canvas::core::ClipId>& selected_clip_ids() const { return selection_.ids(); }

    [[nodiscard]] double frames_per_pixel() const { return frames_per_pixel_; }

    // True when a transition bubble has been clicked and is selected, so
    // Delete/Backspace should clear that transition instead of deleting a clip.
    [[nodiscard]] bool has_selected_transition() const { return selected_transition_.valid; }
    // Clears the selected transition by emitting delete_transition_requested.
    // Returns true when a transition was selected (and the signal emitted).
    bool delete_selected_transition();

signals:
    void playhead_moved(int64_t frame);
    // Emitted when a scrubbing gesture is released at `frame`, so committers can
    // decode and display the crisp full-resolution frame for the settled position.
    void playhead_committed(int64_t frame);
    void clip_selected(const canvas::core::Clip* clip);
    void clip_moved(const canvas::core::Clip* clip, int64_t new_tl_in, canvas::core::Track::Kind dst_kind,
                    int dst_track);
    void blade_requested(const canvas::core::Clip* clip, int64_t frame);
    void range_selected(int64_t in, int64_t out);
    // Emitted when a drag selection covers one or more clips (both video and
    // audio tracks). Carries the ids of every clip overlapping the selection.
    void clips_range_selected(std::vector<canvas::core::ClipId> clip_ids);
    // Emitted when a video clip is dragged upward past the top video track: the
    // handler auto-creates a new video channel (and an audio channel for a
    // linked mate) and moves the clip into it. `tl_in` is the snapped drop
    // position so the move already happened in the model when control returns;
    // the widget then re-acquires its freshly rebuilt clip items and continues
    // the drag on the new track.
    void new_upper_track_requested(canvas::core::ClipId clip_id, int64_t tl_in);
    void media_dropped(int media_id, int64_t frame);
    void media_files_dropped(const QStringList& paths, int64_t frame);
    void unlink_requested(canvas::core::Track::Kind kind, int track_index, canvas::core::ClipId id);
    void link_requested(canvas::core::Track::Kind kind, int track_index, canvas::core::ClipId id);
    void add_track_requested(canvas::core::Track::Kind kind);
    void delete_track_requested(canvas::core::Track::Kind kind, int track_index);
    // Emitted from the clip context menu's "Add Transition >" submenu when the
    // user chooses a transition type (with its default duration in frames).
    void transition_requested(const canvas::core::Clip* clip, canvas::core::TransitionType type,
                              int64_t duration);
    // Emitted when the user chooses an IN (leading-edge) fade-in transition on a
    // clip from its context menu. Independent of the OUT transition: fades the
    // clip in from black over `duration` frames at its head.
    void transition_in_requested(const canvas::core::Clip* clip, canvas::core::TransitionType type,
                                 int64_t duration);
    // Emitted from the clip context menu when the user clears a transition.
    void clear_transition_requested(const canvas::core::Clip* clip);
    // Emitted when the user clears a clip's IN (leading-edge) transition.
    void clear_transition_in_requested(const canvas::core::Clip* clip);
    // Emitted when the user finishes dragging/resizing the transition handle on
    // a cut to a new duration (in frames). The outgoing clip receives the change.
    void transition_resized(const canvas::core::Clip* clip, int64_t duration);
    // Emitted when the user finishes dragging/resizing the IN (leading-edge)
    // transition handle at a clip's start to a new duration (in frames).
    void transition_in_resized(const canvas::core::Clip* clip, int64_t duration);
    // Emitted from the cut context menu's "Delete Through Edit" action, telling
    // the core to merge the outgoing clip (and the clip adjacent to its OUT edge)
    // back into one continuous clip.
    void delete_through_edit_requested(const canvas::core::Clip* out_clip);
    // Emitted when the user presses Delete/Backspace with a transition bubble
    // selected. For a cut bubble `b` is the incoming clip and both edges at the
    // edit point are cleared (A's OUT, B's IN); for a single-clip edge bubble
    // `b` is null and `in_edge` selects the IN (head) edge, else the OUT edge.
    void delete_transition_requested(const canvas::core::Clip* a, const canvas::core::Clip* b,
                                     bool in_edge);

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
    void dragEnterEvent(QDragEnterEvent* event) override;
    void dragMoveEvent(QDragMoveEvent* event) override;
    void dropEvent(QDropEvent* event) override;

private slots:
    void on_thumbnail_ready(uint64_t id, const QImage& image);
    void on_waveform_ready(uint64_t id, const QImage& image);

private:
    void rebuild_timeline();
    void draw_ruler();
    void draw_timecode_bar();
    void draw_tracks();
    // True once the timeline holds at least one clip on any track. Until then
    // the track rows stay hidden and an empty-state placeholder is drawn instead
    // (Resolve keeps the track headers invisible until footage lands).
    bool has_timeline_content() const;
    void draw_empty_state();
    void draw_disabled_marks();
    void draw_playhead();
    void draw_minimap();
    void update_minimap_viewport();
    void update_playhead_position(int64_t frame);
    void request_clip_thumbnails();
    double track_top(int track_index, int v_count) const;
    // Height (px) of the track row with the given flat index.
    double track_height(int track_index, int v_count) const;
    // Flat track index of the row at screen row `s` (0 = the top-most visible
    // row). Video rows descend Vn..V1, then audio rows ascend A1... on screen.
    // Screen row `s == v_count` is the Video/Audio section divider band (a fixed
    // pseudo-row owned by neither section); it maps to -1 (no flat track).
    static int flat_of_screen_row(int s, int v_count) noexcept {
        if (s < v_count) return v_count - 1 - s;
        if (s == v_count) return -1;
        return v_count + (s - v_count - 1);
    }
    // Scene Y of the top of the top-most video track row (below the ruler and
    // the top padding).
    double tracks_origin_y() const;
    // Total content height (ruler/minimap + rows + section band + paddings) for
    // the scene rect.
    double tracks_content_height(int v_count, int a_count) const;
    // Scene Y of the spacer divider above the top-most track row (the top of the
    // top padding strip).
    double tracks_stack_top() const;
    // Scene Y of the divider below the bottom-most track row (the bottom of the
    // last row; the bottom padding strip hangs below it).
    double tracks_stack_bottom(int v_count, int a_count) const;
    // Scene Y of the resize boundary `edge` in screen order. The Video/Audio
    // divider band is a screen element too: 0 = spacer above the top-most row,
    // `edge == v_count` = the band's TOP edge (V1's bottom resize), `edge ==
    // v_count + 1` = the band's BOTTOM edge (A1's top resize), `v_count+a_count+1`
    // = spacer below the bottom-most audio row.
    double edge_y(int edge, int v_count, int a_count) const;
    // True when the scene-space Y is inside the fixed Video/Audio divider band
    // (strictly between its top and bottom edges, so the band's own edges remain
    // plain row-resize handles for V1 and A1).
    bool in_section_divider_band(double scene_y, int v_count, int a_count) const;
    // Resize-zone hit test in the track-header strip: returns a boundary index
    // in screen order (0 = divider above the top-most row, `v_count+a_count+1` =
    // divider below the bottom-most row, otherwise the boundary between two
    // rows), or -1 when the scene-space Y is not near any boundary.
    int header_resize_target(double scene_y, int v_count, int a_count) const;
    void sync_track_heights();
    void set_track_height(int flat_track, int v_count, double height);
    // Given a scene-space Y, returns the flat track index (video tracks first,
    // then audio) or -1 if the point is not over any track row.
    int track_at_y(double scene_y, int v_count) const;
    // Converts a flat track index to a per-kind index (0..kind_count-1).
    int kind_track_index(int flat_track, int v_count) const;
    int64_t frame_at_x(int x) const;
    int64_t snap_frame(int64_t frame) const;
    void scrub_to_frame(int64_t frame);
    void update_blade_preview(int64_t frame);
    void hide_blade_preview();
    void update_snap_indicator(bool snapped, int64_t frame);
    void hide_snap_indicator();
    void apply_selection_highlight();

    // --- Transition cut handle -------------------------------------------------
    // A cut (edit point) is where two adjacent clips touch on the same track. A
    // single-clip edge (clip start for an IN fade, clip end for an OUT fade) is
    // also a transition target so the same hover editor works there.
    enum class Edge { Cut, Start, End };
    struct CutTarget {
        const canvas::core::Clip* a = nullptr;  // outgoing (left) / primary clip
        const canvas::core::Clip* b = nullptr;  // incoming (right) clip; null for single edges
        canvas::core::Track::Kind kind = canvas::core::Track::Kind::Video;
        int track_index = 0;                // flat track index
        int64_t cut_frame = 0;              // edit point frame (A.tl_out, B.tl_in, or tl boundary)
        Edge edge = Edge::Cut;
        [[nodiscard]] bool valid() const noexcept { return a != nullptr; }
        [[nodiscard]] bool is_cut() const noexcept { return b != nullptr; }
    };
    // Finds the cut (and its two neighbouring clips) under `scene_pos`, within a
    // horizontal tolerance band around the edit point. Returns an invalid target
    // when the pointer is not near a cut.
    CutTarget cut_at_scene_pos(const QPointF& scene_pos) const;
    // Show/hide and rebuild the hover transition editor (overlay + icon +
    // duration label) for the current cut target / duration.
    void update_transition_hover(const QPointF& scene_pos);
    void hide_transition_handle();
    void rebuild_transition_handle();
    // Drag the handle's left/right edge to grow/shrink the transition duration.
    // Commit (emit transition_resized) only on release.
    void press_transition_handle(const QPointF& scene_pos);
    void move_transition_handle(const QPointF& scene_pos);
    void release_transition_handle();
    // Starts a bubble-initiated resize drag once the pointer travels past the
    // platform drag threshold (a plain bubble click only selects).
    void engage_transition_drag(const QPointF& scene_pos);
    // Largest legal transition duration for the target (bounded by the shorter
    // of the two neighbouring clips).
    static int64_t transition_max_duration(const CutTarget& t);
    // Persistent glass bubbles drawn on every clip edge that carries a
    // transition (both video clips and their linked audio mates). Pressing a
    // bubble opens the transition editor and starts a resize drag; snapping to
    // the favourite presets (14/30/60/120) stays active for that drag.
    struct TransitionBubble;
    bool maybe_press_transition_bubble(const QPointF& scene_pos);
    void add_transition_bubbles();
    // Records `transition_bubbles_[index]` as the selected transition (survives
    // rebuilds by id) and re-highlights the pill(s).
    void select_transition_bubble(std::size_t index);
    void clear_selected_transition();
    // Re-syncs one bubble's geometry + label to `duration_frames` (live during
    // a bubble drag; anchored to the clip's IN/OUT boundary).
    void refresh_transition_bubble(TransitionBubble& b, int64_t duration_frames);
    void open_transition_editor_for_clip(const canvas::core::Clip* clip, int64_t frame,
                                         bool in_edge);
    // Opens the editor for a cut transition spanning two abutting clips (one
    // bubble centered on the edit point, half over each clip).
    void open_transition_editor_for_cut(const canvas::core::Clip* a,
                                        const canvas::core::Clip* b, int64_t cut_frame);

    // Which overlay edge the pointer is over (index into the two resize edges).
    static constexpr int kTransitionEdgeNone = -1;
    static constexpr int kTransitionEdgeLeft = 0;
    static constexpr int kTransitionEdgeRight = 1;
    // Horizontal tolerance (px) around the cut for hover detection.
    static constexpr double kCutHoverTolerancePx = 8.0;
    // Minimum transition duration in frames.
    static constexpr int64_t kMinTransitionFrames = 1;

    const canvas::core::Sequence* sequence_ = nullptr;
    double fps_ = 30.0;
    double frames_per_pixel_ = 1.0;
    int64_t playhead_frame_ = 0;
    Tool current_tool_ = Tool::Select;
    bool snap_enabled_ = true;
    ThumbnailService* thumbnail_service_ = nullptr;
    uint64_t next_thumb_id_ = 1;
    std::unordered_map<canvas::core::MediaId, MediaMeta> media_paths_;

    QGraphicsScene scene_;
    QGraphicsLineItem* playhead_item_ = nullptr;
    QGraphicsRectItem* selection_rect_ = nullptr;
    QGraphicsRectItem* minimap_viewport_ = nullptr;
    QGraphicsRectItem* minimap_background_ = nullptr;
    // Ruler + minimap rendered as a viewport-pinned overlay: scrollContentsBy()
    // snaps the group back to the current vertical scroll value so the timecode
    // strip never slides up/down when the timeline content pans.
    QGraphicsItemGroup* top_pinned_ = nullptr;
    // Live playhead-position readout drawn over the pinned timecode bar.
    QGraphicsTextItem* timecode_item_ = nullptr;
    // Scene room reserved above the track area so the divider-band pan can move
    // the content DOWN (grab-and-follow) as well as up. Set per rebuild from
    // kPanDownRoomMin scaled by the viewport height; also the parked scroll value
    // that puts the track area right below the pinned ruler strip.
    double pan_down_room_ = kPanDownRoomMin;

    struct ClipCell {
        QGraphicsPixmapItem* item = nullptr;
        uint64_t request_id = 0;
    };
    struct ClipItem {
        QGraphicsRectItem* rect = nullptr;      // invisible geometry / hit-test
        QGraphicsPathItem* shell = nullptr;     // visible semi-rounded clip block (fill only)
        QGraphicsPathItem* outline = nullptr;   // clip-bound stroke, inset so it never overlaps a neighbour
        QGraphicsPathItem* label_bar = nullptr; // visible semi-rounded label strip
        QGraphicsTextItem* text = nullptr;
        std::vector<ClipCell> cells;
        const canvas::core::Clip* clip = nullptr;
        int track_index = 0;
        canvas::core::Track::Kind track_kind = canvas::core::Track::Kind::Video;
    };
    std::vector<ClipItem> clip_items_;

    ClipItem* find_linked_mate(ClipItem* item);
    void position_clip_at(ClipItem& item, int64_t tl_in);
    // Re-locates `dragged_clip_`/`drag_mate_` after an auto-track create rebuilt
    // the scene mid-drag. Clip ids survive the rebuild, so `id` (primary) and
    // `mate_id` (press-time linked mate, 0 if none) find themselves again; `mate_id`
    // is a fallback for when the link was severed by moving onto a fresh track.
    // `pointer_frame` rebases the drag session at the new position/track.
    bool reacquire_dragged_clip(canvas::core::ClipId id, canvas::core::ClipId mate_id,
                                int64_t pointer_frame);
    // Appends the linked mate of each given clip id so that selecting (or
    // deleting) one half of a linked A/V pair selects both halves.
    std::vector<canvas::core::ClipId> expand_with_mates(const std::vector<canvas::core::ClipId>& ids);

    struct TrackHeader {
        QGraphicsRectItem* background = nullptr;
        QGraphicsTextItem* label = nullptr;
        QGraphicsSimpleTextItem* badge = nullptr;
        QGraphicsItem* badge_pill = nullptr;
        QGraphicsTextItem* count = nullptr;
        QGraphicsPixmapItem* lock_icon = nullptr;
        QGraphicsPixmapItem* solo_icon = nullptr;
        QGraphicsPixmapItem* mute_icon = nullptr;
        QGraphicsPixmapItem* view_mode_icon = nullptr;
        QGraphicsTextItem* channel_badge = nullptr;
    };
    std::vector<TrackHeader> video_track_headers_;
    std::vector<TrackHeader> audio_track_headers_;

    QPointF drag_start_pos_;
    QPointF drag_scene_start_;          // scene coords at press; marquee anchor
    bool marquee_full_height_ = false;  // true when press was in the header strip
    QPointF drag_press_pos_;  // press anchor; drag engages only past the threshold
    int64_t drag_start_frame_ = 0;
    double drag_grab_offset_px_ = 0.0;
    ClipItem* dragged_clip_ = nullptr;
    ClipItem* drag_mate_ = nullptr;
    int original_track_index_ = 0;

    // Per-track row heights (flat: video 0..v-1, then audio v..total-1).
    std::vector<double> video_track_heights_;
    std::vector<double> audio_track_heights_;
    // Live header-edge resize drag state. `resize_edge_` is a boundary index in
    // screen order: 0 = the spacer divider above the top-most row, `resize_total_`
    // = the spacer divider below the bottom-most row, interior edges sit between
    // two adjacent rows. On drag the grabbed divider follows the cursor; interior
    // edges reallocate the two neighbours (above -= delta, below += delta, both
    // clamped), stack edges grow/shrink the empty paddings.
    bool resizing_track_ = false;
    int resize_edge_ = -1;
    int resize_total_ = 0;
    double resize_start_y_ = 0.0;    // the divider's scene Y at press (drag anchor)
    double resize_above_start_ = 0.0; // above row height / bottom pad at press
    double resize_below_start_ = 0.0; // below row height / top pad at press
    bool track_resize_cursor_shown_ = false;

    // Video/Audio section-divider PAN state. The wide divider that separates the
    // video stack from the audio stack is a grab-and-scroll handle: dragging it
    // moves the ENTIRE timeline (rows + header column) up and down as one unit,
    // it never resizes anything. The grabbed content follows the cursor, i.e.
    // the vertical scroll offset decreases when dragging down.
    bool pan_dragging_ = false;
    double pan_anchor_viewport_y_ = 0.0;  // viewport Y at press (drag anchor)
    int pan_start_scroll_ = 0;            // vertical scrollbar value at press

    // Stack paddings: empty strips above the top-most and below the bottom-most
    // row that leave room for a drag-select marquee (Resolve exposes these even
    // when every track is short). Edited live by the stack-edge resize dividers.
    double track_v_pad_top_ = kTrackVPad;
    double track_v_pad_bottom_ = kTrackVPad;
    bool is_dragging_ = false;
    // One-shot latch: while a drag is active, that drag may auto-create a new
    // upper track channel a single time (drag-to-the-top is a deliberate
    // gesture, not a continuous rollover).
    bool promote_latched_ = false;

    // Clip-drag session state + math — Qt-free (timeline_drag module): grab
    // offset, track targeting and the release commit decision. The widget keeps
    // the QGraphicsView plumbing (events, mapToScene, painting, snap cue).
    timeline_drag::DragController drag_ctrl_;
    bool is_selecting_range_ = false;
    int64_t range_start_frame_ = 0;
    bool is_scrubbing_ = false;
    QGraphicsLineItem* blade_preview_item_ = nullptr;
    QGraphicsLineItem* snap_indicator_item_ = nullptr;
    std::vector<canvas::core::ClipId> selected_clip_ids_;

    // Selection state + math — Qt-free (timeline_selection module), the widget
    // re-paints the scene from it via apply_selection_highlight().
    timeline_selection::SelectionState selection_;

    // Transition cut-handle editor state (see update_transition_hover).
    bool transition_handle_visible_ = false;
    bool transition_handle_dragging_ = false;
    int transition_drag_edge_ = kTransitionEdgeNone;
    int64_t transition_handle_duration_ = 0;  // active/final duration in frames
    int64_t transition_drag_anchor_frame_ = 0;
    // The overlay's current span (frames) across the cut. Symmetric around the
    // cut by default; one edge moves during a resize drag.
    int64_t transition_handle_left_frame_ = 0;
    int64_t transition_handle_right_frame_ = 0;
    CutTarget transition_target_;
    QGraphicsPathItem* transition_overlay_ = nullptr;
    QGraphicsItem* transition_icon_ = nullptr;
    QGraphicsTextItem* transition_dur_label_ = nullptr;
    // Every scene item currently created by the transition handle, removed
    // together when the handle is hidden.
    QList<QGraphicsItem*> transition_items_;

    struct TransitionBubble {
        canvas::core::ClipId clip_id = 0;   // a-side clip (edge owner for non-cut bubbles)
        canvas::core::ClipId b_clip_id = 0; // b-side clip; nonzero for a cut bubble
        int64_t frame = 0;              // boundary/cut frame (tl_in or tl_out)
        bool in_edge = false;           // true -> transition_in (edge bubbles only)
        bool cut = false;               // true -> one bubble centered across two clips
        double y = 0.0;                 // bubble top in scene coords (clip top)
        double h = 0.0;                 // bubble height (full clip/channel height)
        QRectF hit;
        QGraphicsPathItem* pill = nullptr;
        QGraphicsTextItem* label = nullptr;
    };
    // Persistent glass bubbles owned by rebuild(); cleared with the scene.
    std::vector<TransitionBubble> transition_bubbles_;
    // Active only while dragging via a bubble: each move snaps the duration to
    // the nearest favourite preset so the live label settles on a clean timbre.
    bool transition_snap_presets_ = false;
    // A bubble press is "armed": the selection happens immediately, but the
    // resize editor + drag only engage once the pointer actually travels (a
    // plain click must never edit or create a transition, only select it so
    // Delete can remove it).
    bool transition_press_armed_ = false;
    QPointF transition_press_pos_;
    const canvas::core::Clip* transition_press_a_ = nullptr;
    const canvas::core::Clip* transition_press_b_ = nullptr;
    int64_t transition_press_frame_ = 0;
    bool transition_press_in_edge_ = false;

    // The transition currently selected by clicking a bubble (Delete removes
    // it). Stored by clip id so it survives rebuilds of the scene.
    struct SelectedTransition {
        canvas::core::ClipId a = 0;  // outgoing/primary clip
        canvas::core::ClipId b = 0;  // incoming clip; 0 for a single-clip edge
        bool in_edge = false;    // true -> transition_in edge; else transition_out
        bool valid = false;
    };
    SelectedTransition selected_transition_;
};

}