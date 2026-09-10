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
#include <unordered_set>
#include <vector>
#include <algorithm>

#include "canvas/core/timeline/model.hpp"
#include "canvas/core/timeline/audio_mix.hpp"
#include "Widgets/timeline_selection.hpp"
#include "Widgets/timeline_drag.hpp"
#include "Widgets/transition_handle_editor.hpp"

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
    double fps = 0.0;
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
    // Re-shapes the paint clip during a live trim preview; children (filmstrip,
    // shell, outline, label) are re-laid-out by the caller to fit the new rect.
    void set_shape(const QRectF& scene_rect) { rect_ = scene_rect; prepareGeometryChange(); }
    void paint(QPainter*, const QStyleOptionGraphicsItem*, QWidget*) override {}

private:
    QRectF rect_;
};

inline constexpr int kSceneMargin = 8;
// The filmstrip pitch. Denser pitch = more distinct frame samples per clip,
// and it moves continuously with zoom so each notch re-serves thumbnails.
inline constexpr int kFilmstripCellWidth = 24;

QPixmap scaled_fill(const QImage& img, int w, int h);

class TimelineWidget final : public QGraphicsView {
    Q_OBJECT

public:
    enum class Tool { Select, Trim, Blade };
    // Which edge of a clip an edge-drag TRIM moves (Head = left/tl_in/src_in,
    // Tail = right/tl_out/src_out).
    enum class TrimEdge { Head, Tail };

    static constexpr int kTrackHeaderWidth = 96;
    static constexpr int kRulerHeight = 30;
    static constexpr int kMinimapHeight = 24;
    static constexpr int kTrackGap = 4;
    // Per-track heights: each row keeps its own size, set by dragging a header
    // edge. kTrackVPad is the empty strip above/below the stack that leaves room
    // to start a drag-select marquee.
    static constexpr double kDefaultTrackHeight = 60.0;
    static constexpr double kMinTrackHeight = 44.0;
    static constexpr double kMaxTrackHeight = 200.0;
    // Empty strips above the top video row and below the bottom audio row; the
    // stack-edge resize dividers edit the two paddings, which always leave room
    // to start a drag-select marquee.
    static constexpr double kTrackVPad = 14.0;
    static constexpr double kMinTrackVPad = 2.0;
    static constexpr double kMaxTrackVPad = 200.0;
    // Height of the placeholder panel shown when the timeline has no clips yet
    // (track rows stay fully hidden until a video/audio clip is dragged in).
    static constexpr double kEmptyStateHeight = 120.0;
    // Half-height (px) grab zone around any row-resize divider.
    static constexpr double kResizeGrabHalf = 5.0;
    // The fixed Video/Audio section divider band that separates V1 from A1: it
    // is owned by neither section (V1/A1 cannot resize into it) and its interior
    // is the grab-and-scroll pan handle for the whole timeline. Its edges are
    // V1's and A1's resize boundaries, with a clean pan grip between the zones.
    static constexpr double kSectionDividerHeight = 11.0;
    // Vertical panning is grab-and-follow: pulling the divider UP seats the
    // channels under the ruler and stops (that parked scroll value is the range
    // maximum); pulling DOWN sinks them into a small, finite room below the
    // tracks — the timeline stays compact instead of reading as an endless void.
    static constexpr double kVerticalPanTailMin = 240.0;
    // Depth of the "sink room" below the parked position that the divider pulls
    // rows into. Fixed and modest so the area under the tracks never dwarfs the
    // content on tall windows.
    static constexpr double kPanDownRoomMin = 240.0;
    // Pinned "current time" readout (HH:MM:SS:FF) glued above the minimap/ruler;
    // the whole pinned strip (bar + minimap + ruler) rides in top_pinned_.
    static constexpr double kTimecodeBarHeight = 24.0;
    static constexpr double kMinFramesPerPixel = 0.04;
    static constexpr double kMaxFramesPerPixel = 1500.0;
    // Interactive zoom is a percentage of the 100% baseline = one frame per
    // pixel; zoom_fit() is exempt so long sequences still fit. The zoom-IN end
    // is a hard 2500% (0.04 frames per pixel). The zoom-OUT end is dynamic:
    // with a sequence loaded it runs ~5x past "whole timeline fits the viewport"
    // (see interactive_floor_percent()); kZoomMinPercent is only the fallback
    // floor until a sequence exists.
    static constexpr double kDefaultFramesPerPixel = 1.0;
    static constexpr double kZoomMinPercent = 4.0;
    static constexpr double kZoomMaxPercent = 2500.0;
    // Height of the flat label bar (filename strip) at the bottom of each clip.
    static constexpr double kClipLabelHeight = 18.0;
    static constexpr double kClipOutlineW = 1.5;   // clip-bound stroke; inset by pen/2 so it never overhangs
    static constexpr double kClipSelectedOutlineW = 2.2;

    explicit TimelineWidget(QWidget* parent = nullptr);
    ~TimelineWidget() override;

    void set_sequence(const canvas::core::Sequence* sequence);
    void set_fps(double fps);
    void set_playhead_position(int64_t frame);
    void set_tool(Tool tool);
    void set_snap_enabled(bool enabled);
    // Magnetic playhead position for a raw frame: clamps to the nearest clip
    // edge / bookmark within the pixel radius, then the grid. Public so the
    // transport-bar overview scrub slider snaps to the same cut points as the
    // timeline ruler.
    [[nodiscard]] int64_t snap_frame(int64_t frame) const;
    // When true the view follows the playhead (scrolls to keep it visible)
    // during playback and explicit playhead jumps. Manual scroll/zoom navigation
    // flips this off so the user is never yanked back to the playhead; the
    // explicit jumps (ruler/minimap click, scrub, transport to-start/to-end,
    // play) re-enable it.
    void set_follow_playhead(bool follow) { follow_playhead_ = follow; }
    [[nodiscard]] bool follow_playhead() const { return follow_playhead_; }
    void zoom_fit();
    void zoom_in();
    void zoom_out();
    void set_frames_per_pixel(double fpp);
    void set_zoom_percent(double percent);
    // The lowest interactive zoom (as a percent of the 100% baseline), roughly
    // 5x past the "whole timeline fits the viewport" point. Zooming out to this
    // floor always brings the tail of the longest clip into view (and well
    // beyond). Falls back to kZoomMinPercent with no sequence. Also the lower
    // end of the zoom slider's mapping range.
    [[nodiscard]] double interactive_floor_percent() const;
    [[nodiscard]] double zoom_percent() const { return kDefaultFramesPerPixel / frames_per_pixel_ * 100.0; }
    void set_thumbnail_service(ThumbnailService* service);
    void set_media_paths(std::unordered_map<canvas::core::MediaId, MediaMeta> paths);
    // Highlight a set of selected clip ids (paints their shells blue).
    void set_selection(const std::vector<canvas::core::ClipId>& ids);
    void clear_selection();
    // The ids currently selected in the timeline (visual selection).
    [[nodiscard]] const std::vector<canvas::core::ClipId>& selected_clip_ids() const { return selection_.ids(); }

    [[nodiscard]] double frames_per_pixel() const { return frames_per_pixel_; }

    // Lane a media drop at `scene_y` would target: the existing track of the
    // media's kind under the cursor, or the per-kind index of a NEW track to
    // create at the drop position (>= current count of that kind) when the drop
    // lands on empty space. Cross-section drops clamp to the first track of the
    // media's kind. `index` is a per-kind slicing index (V1/A1 = 0, V2/A2 = 1, ...).
    struct DropLane {
        canvas::core::Track::Kind kind = canvas::core::Track::Kind::Video;
        int index = 0;
    };
    DropLane resolve_drop_lane(double scene_y, canvas::core::Track::Kind media_kind) const;

    // True when a transition bubble is selected, so Delete/Backspace clears the
    // transition instead of deleting a clip.
    [[nodiscard]] bool has_selected_transition() const { return selected_transition_.valid; }
    // Outgoing clip id of the selected transition bubble (0 when none selected).
    [[nodiscard]] canvas::core::ClipId selected_transition_a() const { return selected_transition_.a; }
    // Incoming clip id of a cut bubble; 0 for a single-clip edge bubble.
    [[nodiscard]] canvas::core::ClipId selected_transition_b() const { return selected_transition_.b; }
    // Dock height (px) that shows every channel flush at the parked scroll
    // position. MainWindow grows the timeline dock to this when the channel
    // count changes so the timeline height follows its content instead of being
    // severed inside a fixed 265px dock.
    [[nodiscard]] int desired_timeline_height() const;
    // True when the timeline holds at least one clip anywhere; false keeps the
    // dock at its compact floor (no channels have been populated yet).
    [[nodiscard]] bool has_clips() const { return has_timeline_content(); }
    // Clears the selected transition by emitting delete_transition_requested.
    // Returns true when a transition was selected (and the signal emitted).
    bool delete_selected_transition();

    // One clip's batched drag result. `new_tl_in` is that clip's snapped target
    // timeline position; `kind`/`track_index` identify the track (the primary
    // clip may have crossed tracks; the others keep their own).
    struct MovedClip {
        canvas::core::ClipId id = 0;
        int64_t new_tl_in = 0;
        canvas::core::Track::Kind kind = canvas::core::Track::Kind::Video;
        int track_index = 0;  // PER-KIND index (audio = index within audio_tracks)
    };

    // Resolve-style magnetic snap target set for the CURRENT drag: every OTHER
    // clip's in/out edge plus the bookmarks (and the playhead when asked),
    // sorted + deduped, EXCLUDING `exclude` (the dragged clip, its mate and
    // every co-selected clip move together and must not self-attract). Rebuilt
    // at drag press / re-acquire; cleared at release.
    std::vector<int64_t> collect_snap_targets(
        const std::vector<canvas::core::ClipId>& exclude,
        bool include_playhead) const;

signals:
    void playhead_moved(int64_t frame);
    // Emitted when a scrubbing gesture releases at `frame`, so committers can
    // decode and display the crisp full-resolution frame for the settled position.
    void playhead_committed(int64_t frame);
    void clip_selected(const canvas::core::Clip* clip);
    void clip_moved(const canvas::core::Clip* clip, int64_t new_tl_in, canvas::core::Track::Kind dst_kind,
                    int dst_track);
    // Emitted when a multi-clip drag releases: every selected clip that actually
    // moved (same snapped delta), with its target tl_in and current track.
    void clips_moved(std::vector<MovedClip> clips);
    // Volume-line drag (Phase 5): emitted on every live move with the preview dB
    // (the phase 4/preview path re-renders the waveform + re-mixes playback), and
    // once on release with the settled dB (commit applies it to every selected
    // audio target, mirroring the inspector Volume path).
    void volume_line_preview(float db);
    void volume_line_committed(float db);
    // Emitted when a clip edge-drag trim finishes: `edge` is Head or Tail and
    // `new_frame` is the trimmed tl edge. The handler runs the core trim edit op
    // (which needs the media duration, resolved by clip id here).
    void clip_trimmed(const canvas::core::Clip* clip, TrimEdge edge, int64_t new_frame);
    void blade_requested(const canvas::core::Clip* clip, int64_t frame);
    void range_selected(int64_t in, int64_t out);
    // Emitted when a drag selection covers one or more clips across both track
    // kinds; carries every overlapping clip id.
    void clips_range_selected(std::vector<canvas::core::ClipId> clip_ids);
    // Emitted when a video clip is dragged past the top video track: the handler
    // auto-creates a new video channel (and an audio channel for a linked mate)
    // and moves the clip into it. `tl_in` is the snapped drop position (the move
    // already happened in the model); the widget re-acquires the rebuilt items
    // and continues the drag on the new track.
    void new_upper_track_requested(canvas::core::ClipId clip_id, int64_t tl_in);
    void media_dropped(int media_id, int64_t frame, double scene_y);
    void media_files_dropped(const QStringList& paths, int64_t frame, double scene_y);
    void unlink_requested(canvas::core::Track::Kind kind, int track_index, canvas::core::ClipId id);
    void link_requested(canvas::core::Track::Kind kind, int track_index, canvas::core::ClipId id);
    void add_track_requested(canvas::core::Track::Kind kind);
    void delete_track_requested(canvas::core::Track::Kind kind, int track_index);
    // Emitted when the user clicks a track header's M/S/L icon: `on` is the
    // DESIRED state (the inverse of the track's current flag). Carries
    // Track::Kind + index so a single handler can toggle audio as well as
    // video (video headers only offer the lock icon).
    void track_mute_toggled(canvas::core::Track::Kind kind, int track_index, bool on);
    void track_solo_toggled(canvas::core::Track::Kind kind, int track_index, bool on);
    void track_lock_toggled(canvas::core::Track::Kind kind, int track_index, bool on);
    // Emitted from the clip context menu's "Add Transition" submenu.
    void transition_requested(const canvas::core::Clip* clip, canvas::core::TransitionType type,
                              int64_t duration);
    // Emitted when the user picks an IN (leading-edge) fade-in on a clip from its
    // context menu; independent of the OUT transition.
    void transition_in_requested(const canvas::core::Clip* clip, canvas::core::TransitionType type,
                                 int64_t duration);
    // Emitted when the user clears a transition.
    void clear_transition_requested(const canvas::core::Clip* clip);
    // Emitted when the user clears a clip's IN (leading-edge) transition.
    void clear_transition_in_requested(const canvas::core::Clip* clip);
    // Emitted when the transition handle drag finishes with a new duration;
    // the outgoing clip receives the change.
    void transition_resized(const canvas::core::Clip* clip, int64_t duration);
    // Emitted when the IN (leading-edge) transition handle drag finishes.
    void transition_in_resized(const canvas::core::Clip* clip, int64_t duration);
    // Emitted from the cut context menu's "Delete Through Edit" action, telling
    // the core to merge the outgoing clip (and the clip adjacent to its OUT edge)
    // back into one continuous clip.
    void delete_through_edit_requested(const canvas::core::Clip* out_clip);
    // Emitted when Delete/Backspace is pressed with a transition bubble selected.
    // For a cut bubble `b` is the incoming clip and both edges clear; for a
    // single-clip edge bubble `b` is null and `in_edge` selects the IN edge.
    void delete_transition_requested(const canvas::core::Clip* a, const canvas::core::Clip* b,
                                     bool in_edge);
    // Emitted when a transition bubble is selected, so the inspector can switch
    // to the Transition page. For a cut bubble `b` is the incoming clip; for a
    // single-clip edge bubble `b` is null and `in_edge` selects the IN edge.
    void transition_selected(canvas::core::ClipId a, canvas::core::ClipId b, bool in_edge);
    // Emitted when the selected transition bubble is cleared (click dodge,
    // deletion, timeline rebuild).
    void transition_selection_cleared();
    // Emitted when the channel count (or the per-track heights) changed enough
    // that the dock should grow/shrink to keep every channel visible. Carries
    // the target dock height in px from desired_timeline_height().
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
    // Watches the scrollbars directly (QGraphicsView does not forward their
    // events to the view), so thumb-dragging / pressing / wheeling a scrollbar
    // disables playhead-follow; wheelEvent() covers viewport wheel scrolling.
    bool eventFilter(QObject* watched, QEvent* event) override;
    void paintEvent(QPaintEvent* event) override;
    void dragEnterEvent(QDragEnterEvent* event) override;
    void dragMoveEvent(QDragMoveEvent* event) override;
    void dropEvent(QDropEvent* event) override;

private slots:
    void on_thumbnail_ready(uint64_t id, const QImage& image);
    void on_waveform_ready(uint64_t id, const QImage& image);

private:
    void rebuild_timeline();
    // Viewport-only relayout: recomputes the scene rect and re-renders the
    // view chrome (background, ruler/minimap/timecode strip, gridlines,
    // divider, playhead) WITHOUT tearing down the clip/track/filmstrip items or
    // re-requesting thumbnails. Clip geometry is anchored to timeline coords
    // (frame + track height), never to the viewport, so a resize never needs
    // the full scene rebuild that made height changes stall on clip-heavy
    // timelines.
    void relayout_scene();
    void draw_ruler();
    void draw_timecode_bar();
    void draw_tracks();
    // True once the timeline holds at least one clip; until then track rows stay
    // hidden and an empty-state placeholder is drawn.
    bool has_timeline_content() const;
    void draw_empty_state();
    void draw_disabled_marks();
    void draw_playhead();
    void draw_minimap();
    void update_minimap_viewport();
    void update_playhead_position(int64_t frame);
    void request_clip_thumbnails();
    // Procedural scene-chrome painters. The ruler ticks+labels and the
    // full-height gridlines used to be one QGraphicsLineItem/QGraphicsTextItem
    // PER TICK across the entire scene width (~10k+ items on a wide timeline),
    // all re-created on every rebuild AND every relayout — the "resizing is
    // slow" / "wide timeline is heavy" cost. They now repaint only the visible
    // x-range (option->exposedRect) from a single item each, so scene item
    // count stays O(viewport chrome) regardless of timeline width.
    class RulerMarksItem;
    class GridlinesItem;
    double track_top(int track_index, int v_count) const;
    // Height (px) of the track row with the given flat index.
    double track_height(int track_index, int v_count) const;
    // Flat track index of the row at screen row `s` (0 = top-most visible row).
    // Screen row `v_count` is the Video/Audio section divider band (a fixed
    // pseudo-row owned by neither section); it maps to -1.
    static int flat_of_screen_row(int s, int v_count) noexcept {
        if (s < v_count) return v_count - 1 - s;
        if (s == v_count) return -1;
        return v_count + (s - v_count - 1);
    }
    // Scene Y of the top of the top-most video track row (below the ruler).
    double tracks_origin_y() const;
    // Total content height (ruler/minimap + rows + section band + paddings).
    double tracks_content_height(int v_count, int a_count) const;
    // Scene Y of the spacer divider above the top-most track row.
    double tracks_stack_top() const;
    // Scene Y just below the bottom-most track row; the bottom padding hangs
    // below it.
    double tracks_stack_bottom(int v_count, int a_count) const;
    // Scene Y of the resize boundary `edge` in screen order: 0 = spacer above
    // the top-most row; `edge == v_count` = the band's top edge (V1's bottom
    // resize); `edge == v_count + 1` = the band's bottom edge (A1's top resize);
    // `v_count+a_count+1` = spacer below the bottom-most audio row.
    double edge_y(int edge, int v_count, int a_count) const;
    // True when the scene Y is strictly inside the Video/Audio divider band
    // (its own edges remain plain row-resize handles for V1 and A1).
    bool in_section_divider_band(double scene_y, int v_count, int a_count) const;
    // Resize-zone hit test in the track-header strip: boundary index in screen
    // order (0 = divider above the top row, `v_count+a_count+1` = divider below
    // the bottom row, else the boundary between two rows), or -1 when not near
    // any boundary.
    int header_resize_target(double scene_y, int v_count, int a_count) const;
    void sync_track_heights();
    void set_track_height(int flat_track, int v_count, double height);
    // Flat track index under `scene_y` (video tracks first, then audio), or -1.
    int track_at_y(double scene_y, int v_count) const;
    // Converts a flat track index to a per-kind index (0..kind_count-1).
    int kind_track_index(int flat_track, int v_count) const;
    int64_t frame_at_x(int x) const;
    // Blade cut frame for a pointer at viewport x: the NEAREST frame to the
    // mouse (llround — no floor left-bias). The razor ignores the playhead and
    // snapping entirely: it cuts exactly where the user points.
    int64_t blade_cut_frame(int x) const;
    void scrub_to_frame(int64_t frame);
    // Cached snap targets for the live drag (see collect_snap_targets); empty
    // between drags.
    std::vector<int64_t> snap_targets_;
    void update_blade_preview(int64_t frame);
    void hide_blade_preview();
    void update_snap_indicator(bool snapped, int64_t frame);
    void hide_snap_indicator();
    void apply_selection_highlight();

    // --- Transition cut handle ---
    // A cut (edit point) is where two adjacent clips touch on the same track; a
    // single-clip edge (IN fade at clip start, OUT fade at clip end) is also a
    // target so the same hover editor works there.
    // Qt-free editor types (transition_handle_editor module): the boundary
    // kinds and the cut target descriptor the handle offers to resize.
    using Edge = transition_editor::Edge;
    using CutTarget = transition_editor::CutTarget;
    // Finds the cut (and its two neighbouring clips) near `scene_pos` within a
    // horizontal tolerance band; invalid when the pointer is not near a cut.
    CutTarget cut_at_scene_pos(const QPointF& scene_pos) const;
    // Show/hide and rebuild the hover transition editor (overlay + icon + label).
    void update_transition_hover(const QPointF& scene_pos);
    void hide_transition_handle();
    void rebuild_transition_handle();
    // Drag the handle's left/right edge to grow/shrink the duration; commit
    // (emit transition_resized) only on release.
    void press_transition_handle(const QPointF& scene_pos);
    void move_transition_handle(const QPointF& scene_pos);
    void release_transition_handle();
    // Engage a bubble-initiated resize drag once the pointer passes the platform
    // drag threshold (a plain bubble click only selects).
    void engage_transition_drag(const QPointF& scene_pos);
    // Persistent glass bubbles drawn on every clip edge that carries a transition
    // (video + linked audio mates); pressing one opens the editor and starts a
    // resize drag with preset snapping kept active.
    struct TransitionBubble;
    bool maybe_press_transition_bubble(const QPointF& scene_pos);
    void add_transition_bubbles();
    // Marks `transition_bubbles_[index]` selected (survives rebuilds by id).
    void select_transition_bubble(std::size_t index);
    void clear_selected_transition();
    // Re-syncs one bubble's geometry + label to `duration_frames` (live during a
    // bubble drag, anchored to the clip's IN/OUT boundary).
    void refresh_transition_bubble(TransitionBubble& b, int64_t duration_frames);
    void open_transition_editor_for_clip(const canvas::core::Clip* clip, int64_t frame,
                                         bool in_edge);
    // Opens the editor for a cut transition spanning two abutting clips (one
    // bubble centered on the edit point, half over each clip).
    void open_transition_editor_for_cut(const canvas::core::Clip* a,
                                        const canvas::core::Clip* b, int64_t cut_frame);

    // Which overlay edge the pointer is over (index into the two resize edges).
    // Drag-edge indices + minimum duration live in the Qt-free editor module.
    static constexpr int kTransitionEdgeNone = transition_editor::kDragEdgeNone;
    static constexpr int kTransitionEdgeLeft = transition_editor::kDragEdgeLeft;
    static constexpr int kTransitionEdgeRight = transition_editor::kDragEdgeRight;
    // Horizontal tolerance (px) around the cut for hover detection.
    static constexpr double kCutHoverTolerancePx = 8.0;
    static constexpr int64_t kMinTransitionFrames = transition_editor::kMinTransitionFrames;

    const canvas::core::Sequence* sequence_ = nullptr;
    double fps_ = 30.0;
    double frames_per_pixel_ = 1.0;
    int64_t playhead_frame_ = 0;
    // Follows the playhead (update_playhead_position centerOn) only while true;
    // see the set_follow_playhead() doc above.
    bool follow_playhead_ = true;
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
    // Unpinned view chrome that scales with the scene rect (full-scene
    // background, ruler gridlines, header divider). Rendered by the draw_*
    // functions and re-rendered by relayout_scene() on resize; lives at a low
    // z so clip/track items always paint above it regardless of insertion order.
    QGraphicsItemGroup* chrome_ = nullptr;
    // Set when sync_track_heights() sees the channel count change so
    // rebuild_timeline() can tell MainWindow the dock must grow/shrink.
    bool track_count_changed_ = false;
    // Content presence (at least one clip) as of the last rebuild, so the
    // empty->content transition can also refit the dock without a channel-count
    // change. A track-height drag is deliberately NOT a refit trigger: the user
    // is hand-tuning one row and the dock must not yank the layout per pixel.
    bool last_had_content_ = false;
    // Ruler + minimap rendered as a viewport-pinned overlay: scrollContentsBy()
    // snaps the group back to the current vertical scroll so the timecode strip
    // never slides when the content pans.
    QGraphicsItemGroup* top_pinned_ = nullptr;
    // Live playhead readout drawn over the pinned timecode bar.
    QGraphicsTextItem* timecode_item_ = nullptr;
    // Room reserved above the track area so the divider-band pan can move the
    // content down (grab-and-follow); also the parked scroll value for the track
    // area right below the pinned ruler strip.
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
        // Audio volume line (Phase 5): a persistent gain readout/drag handle drawn
        // across the waveform box. volume_y0/volume_h define that box (absolute
        // scene top + height) so the drag math stays in the widget.
        QGraphicsLineItem* volume_line = nullptr;
        double volume_y0 = 0.0;
        double volume_h = 0.0;
    };
    std::vector<ClipItem> clip_items_;

    ClipItem* find_linked_mate(ClipItem* item);
    // Phase 8 trim snapping: the MOVING edge of a trim magnets onto the section
    // points (every other clip's in/out + bookmarks + the playhead, minus the
    // trimmed clip and its mate) within the pixel-derived radius. Unlike clip
    // drags, there is NO grid fallback — the trim stays continuous so a
    // frame-precise out can be reached between grid multiples.
    int64_t snap_trim_edge(int64_t raw_edge, ClipItem* clip);
    void position_clip_at(ClipItem& item, int64_t tl_in);
    // Live-resizes a clip's rect (and its linked mate's) so the head/tail edge
    // lands at `edge` (timeline frame) during a trim preview, without touching
    // the model. Both the whole-clip rect and the paint-clip group are adjusted.
    void preview_trim_clip(ClipItem& item, TrimEdge edge, int64_t tl_frame);
    // Edge-drag TRIM hit-test: fills `out_clip`/`out_edge` when `scene_pos` sits
    // within the outer trim band of a clip's head/tail wall (body region only,
    // not the label strip). Takes PRIORITY over transition bubbles/overlay grabs,
    // so a clip that carries a fade can still be regrown by grabbing its wall.
    bool find_trim_edge(const QPointF& scene_pos, ClipItem*& out_clip, TrimEdge& out_edge);
    // Volume-line hit-test (Phase 5): true when `scene_pos` is inside an audio
    // clip's block and within the gain line's thin vertical hit band. Fills
    // `out_clip` on hit.
    bool find_volume_line_hit(const QPointF& scene_pos, ClipItem*& out_clip);
    // Re-locates `dragged_clip_`/`drag_mate_` after an auto-track create rebuilt
    // the scene mid-drag. Clip ids survive the rebuild, so `id` and `mate_id`
    // (press-time linked mate, 0 if none) find themselves again; `pointer_frame`
    // rebases the drag session at the new position/track.
    bool reacquire_dragged_clip(canvas::core::ClipId id, canvas::core::ClipId mate_id,
                                int64_t pointer_frame);
    // Live-drag bubble follow: shifts every transition bubble anchored to a clip
    // in `clip_deltas` (id -> preview tl_in delta) by that clip's delta. Called
    // from the drag move path so bubbles ride along while `position_clip_at`
    // repositions the clip blocks; the commit/rebuild recreates them exactly.
    void shift_transition_bubbles(const std::unordered_set<canvas::core::ClipId>& dragging);
    // Volume-line session (Phase 5): re-anchors one audio clip's gain line at
    // `db` during a drag WITHOUT touching the model (used by the interaction's
    // volume-drag move path). Rescales the clip's waveform the same way, so the
    // spectrum updates live with the volume (floored — never a flat line).
    void set_live_clip_gain(canvas::core::ClipId id, float db);
    // Re-applies the audio clip's committed volume to its waveform pixmap's
    // vertical scale (about the box's vertical center). Keeps the spectrum in
    // sync with the volume after a commit/rebuild re-rendered the waveform.
    void apply_waveform_volume_scale(ClipItem& item, float db);
    // Appends each given id's linked mate so selecting/deleting one half of a
    // linked A/V pair selects both halves.
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

    // Batch-drag snapshot (Phase 3): every selected clip (incl. linked mates)
    // plus its press-time tl_in, snapped at multi-selection press and live-written
    // on each move. On release the moved-set is emitted as clips_moved; the
    // TimelineActions handler de-dups linked pairs before running move_clip.
    std::vector<MovedClip> drag_clip_snapshot_;
    std::vector<int64_t> drag_clip_orig_;
    canvas::core::ClipId drag_primary_id_ = 0;

    // Returns the ClipItem whose clip->id matches, or nullptr.
    ClipItem* find_clip_item(canvas::core::ClipId id);

    // Clip edge-drag TRIM session (Phase 2). A press near a clip's head/tail
    // edge (below any transition bubble) starts a trim; the edge is live-resized
    // to the pointer and committed (emit clip_trimmed) only on release. The core
    // op clamps to the media duration and neighbours, so the preview here just
    // mirrors the pointer position.
    bool trimming_ = false;
    ClipItem* trimmed_clip_ = nullptr;
    TrimEdge trim_edge_ = TrimEdge::Head;
    int64_t trim_start_edge_ = 0;       // clip edge (tl_in or tl_out) at press
    double trim_grab_offset_px_ = 0.0;  // pointer offset within the edge in px

    // Audio volume-line drag session (Phase 5). A press within the line's hit
    // band (Select tool, body region, below the trim-zone) arms a loudness
    // drag: up = louder. Each move emits volume_line_preview(db); release emits
    // volume_line_committed(db). The session NEVER arms the clip move-drag.
    bool volume_drag_armed_ = false;
    bool volume_dragging_ = false;
    canvas::core::ClipId volume_drag_clip_ = 0;
    float volume_drag_db_ = 0.0f;
    // Audio clips the current volume drag previews/commits onto, resolved from
    // the selection at press time (multi-selection: every selected audio target,
    // incl. linked mates). Each move re-anchors ALL of them live; the commit
    // handler resolves the same set so preview == commit.
    std::vector<canvas::core::ClipId> volume_drag_targets_;

    // Per-track row heights (flat: video 0..v-1, then audio v..total-1).
    std::vector<double> video_track_heights_;
    std::vector<double> audio_track_heights_;
    // Live header-edge resize drag state. `resize_edge_` is a boundary index in
    // screen order; interior edges reallocate the two neighbours (above -=
    // delta, below += delta, both clamped), stack edges grow/shrink the paddings.
    bool resizing_track_ = false;
    int resize_edge_ = -1;
    int resize_total_ = 0;
    double resize_start_y_ = 0.0;    // the divider's scene Y at press (drag anchor)
    double resize_above_start_ = 0.0; // above row height / bottom pad at press
    double resize_below_start_ = 0.0; // below row height / top pad at press
    bool track_resize_cursor_shown_ = false;

    // Video/Audio section-divider PAN state: the wide divider is a
    // grab-and-scroll handle that moves the ENTIRE timeline (rows + headers) up
    // and down as one unit — it never resizes anything. Dragging down increases
    // the vertical scroll offset (content follows the cursor).
    bool pan_dragging_ = false;
    double pan_anchor_viewport_y_ = 0.0;  // viewport Y at press (drag anchor)
    int pan_start_scroll_ = 0;            // vertical scrollbar value at press

    // Stack paddings: empty strips above/below the stack that keep room for a
    // drag-select marquee, edited live by the stack-edge resize dividers.
    double track_v_pad_top_ = 15.0;
    double track_v_pad_bottom_ = kTrackVPad;
    bool is_dragging_ = false;
    // One-shot latch: a drag may auto-create an upper track channel a single
    // time (drag-to-the-top is deliberate, not a continuous rollover).
    bool promote_latched_ = false;

    // Clip-drag session state + math — Qt-free (timeline_drag module); the
    // widget keeps the QGraphicsView plumbing (events, mapping, painting).
    timeline_drag::DragController drag_ctrl_;
    bool is_selecting_range_ = false;
    int64_t range_start_frame_ = 0;
    bool is_scrubbing_ = false;
    QGraphicsLineItem* blade_preview_item_ = nullptr;
    QGraphicsLineItem* snap_indicator_item_ = nullptr;
    std::vector<canvas::core::ClipId> selected_clip_ids_;

    // Selection state + math — Qt-free (timeline_selection module); the widget
    // re-paints the scene from it via apply_selection_highlight().
    timeline_selection::SelectionState selection_;

    // Transition cut-handle editor state + math — Qt-free
    // (transition_handle_editor module, splitplan Phase 30); the widget keeps
    // the QGraphicsView plumbing (hover hit-testing, cursor, overlay/icon
    // painting, signal emission, bubble refresh) and delegates the session +
    // frame math to the editor.
    transition_editor::Editor transition_editor_;
    // Deliberately no on-canvas duration text: the bubble/hover overlay shows only
    // its shape + icon, so the timeline never reads as a spreadsheet. Duration is
    // shown in the right-click context menu instead.
    QGraphicsPathItem* transition_overlay_ = nullptr;
    QGraphicsItem* transition_icon_ = nullptr;
    // Every scene item currently created by the transition handle, removed
    // together when the handle is hidden.
    QList<QGraphicsItem*> transition_items_;

    struct TransitionBubble {
        canvas::core::ClipId clip_id = 0;   // a-side clip (edge owner for non-cut bubbles)
        canvas::core::ClipId b_clip_id = 0; // b-side clip; nonzero for a cut bubble
        int64_t frame = 0;              // boundary/cut frame (tl_in or tl_out)
        int64_t dur = 0;                // transition duration (frames) — width source for refresh
        bool in_edge = false;           // true -> transition_in (edge bubbles only)
        bool cut = false;               // true -> one bubble centered across two clips
        double y = 0.0;                 // bubble top in scene coords (clip top)
        double h = 0.0;                 // bubble height (full clip/channel height)
        QRectF hit;
        QGraphicsPathItem* pill = nullptr;
    };
    // Persistent glass bubbles owned by rebuild(); cleared with the scene.
    std::vector<TransitionBubble> transition_bubbles_;
    // A bubble press is "armed": selection is immediate, but the resize editor +
    // drag only engage once the pointer travels (a plain click must only select,
    // so Delete can remove the transition).
    bool transition_press_armed_ = false;
    QPointF transition_press_pos_;
    const canvas::core::Clip* transition_press_a_ = nullptr;
    const canvas::core::Clip* transition_press_b_ = nullptr;
    int64_t transition_press_frame_ = 0;
    bool transition_press_in_edge_ = false;

    // The transition currently selected by clicking a bubble (Delete removes it),
    // stored by clip id so it survives rebuilds.
    struct SelectedTransition {
        canvas::core::ClipId a = 0;  // outgoing/primary clip
        canvas::core::ClipId b = 0;  // incoming clip; 0 for a single-clip edge
        bool in_edge = false;    // true -> transition_in edge; else transition_out
        bool valid = false;
    };
    SelectedTransition selected_transition_;
};

}