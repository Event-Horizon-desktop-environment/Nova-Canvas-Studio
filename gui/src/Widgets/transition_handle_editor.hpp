#pragma once

// TransitionHandleEditor — Qt-free transition cut-handle session state + math
// for the timeline (splitplan Phase 30). The widget keeps the QGraphicsView
// plumbing (hover hit-testing via cut_at_scene_pos, pixel mapping, cursor,
// overlay/icon painting, signal emission, bubble refresh) and delegates the
// editor decision logic here so it can be unit-tested from the headless seam
// (gui/tests) and scanned by scripts/check_qtdep.sh.
//
// The editor owns exactly the state a transition resize needs from hover/press
// to release:
//   * CutTarget — the edit point the handle is offering to resize: a cut across
//     two adjacent clips, or a single clip's Start (IN) / End (OUT) boundary;
//   * duration / left / right — the transition window in sequence frames,
//     symmetric across a cut by default and anchored on the fixed edge while a
//     resize drag moves the grabbed edge;
//   * drag_edge / anchor — which overlay edge is being resized and the opposite
//     (fixed) edge it stays anchored to;
//   * snap — favourite-preset snapping ({14,30,60,120}) used during
//     bubble-initiated drags so the live duration settles on a clean value.
//
// All frame math is clamped to [kMinTransitionFrames, max_duration(target)] and
// replicates the pre-extraction widget behaviour exactly (pure move). No Qt
// headers.

#include <algorithm>
#include <cstdint>
#include <cstdlib>

#include "canvas/core/timeline/model.hpp"

namespace canvas::gui {
namespace transition_editor {

// Boundary kind the handle targets: a Cut spans two abutting clips at their
// shared edit point; Start/End are a single clip's IN/OUT boundary.
enum class Edge { Cut, Start, End };

// The edit point the transition handle is offering to resize. Mirrors the
// widget's CutTarget: `a` is the outgoing (left) / primary clip, `b` the
// incoming (right) clip — null for a single-clip edge; `cut_frame` is the edit
// point in sequence frames (A.tl_out, B.tl_in, or the clip boundary).
struct CutTarget {
    const canvas::core::Clip* a = nullptr;  // outgoing (left) / primary clip
    const canvas::core::Clip* b = nullptr;  // incoming (right) clip; null for single edges
    canvas::core::Track::Kind kind = canvas::core::Track::Kind::Video;
    int track_index = 0;                // flat track index (paint-side info)
    int64_t cut_frame = 0;              // edit point frame (A.tl_out, B.tl_in, or tl boundary)
    Edge edge = Edge::Cut;

    [[nodiscard]] bool valid() const noexcept { return a != nullptr; }
    [[nodiscard]] bool is_cut() const noexcept { return b != nullptr; }
};

// Minimum transition duration in frames.
constexpr int64_t kMinTransitionFrames = 1;

// Drag edge indices (the overlay resize edge the pointer grabbed; index into
// the left/right pair).
constexpr int kDragEdgeNone = -1;
constexpr int kDragEdgeLeft = 0;
constexpr int kDragEdgeRight = 1;

// Favourite durations a bubble-initiated drag snaps to (the live label settles
// on a clean value instead of a free-form frame count).
constexpr int64_t kFavoritePresets[] = {14, 30, 60, 120};

class Editor {
public:
    // Largest legal transition duration: on a cut, bounded by the shorter of
    // the two neighbouring clips; on a single-clip edge, by that clip's own
    // duration.
    static int64_t max_duration(const CutTarget& t);

    // Open/re-open a session for `target`, seeding the duration from `seeded`
    // (the caller computes the seed exactly as the widget did before the split:
    // the clip's existing IN or OUT duration, or a cut's preferred side, with
    // the 6-frame fallback). Returns true when the session actually (re)opened
    // — a call over an already-open identical target returns false so the
    // widget can skip rebuilding its scene items.
    bool open(const CutTarget& target, int64_t seeded);

    // Tear the session down; the widget also removes its painted handle items.
    void close();

    // Begin resizing the given overlay edge (kDragEdgeLeft / kDragEdgeRight),
    // anchored at the opposite edge. `snap` enables favourite-preset snapping
    // for the whole drag (bubble-initiated drags pass true).
    void begin_drag(int edge, bool snap);
    // Follow the pointer to a sequence frame: recompute the duration (clamped
    // to the legal range, preset-snapped when enabled) and re-derive both edges
    // from the frozen anchor.
    void move_to(int64_t pointer_frame);
    // Finish the drag; the widget then commits the duration and hides.
    void end_drag();

    // Whether the target already carries a transition at this boundary — the
    // press gate (a hover preview must never create a transition; adding is
    // right-click-menu only).
    [[nodiscard]] bool has_transition() const;
    // The transition duration currently stored at the target boundary (the
    // widget commits a resize only when the dragged duration differs).
    [[nodiscard]] int64_t stored_duration() const;

    [[nodiscard]] bool visible() const { return visible_; }
    [[nodiscard]] bool dragging() const { return dragging_; }
    [[nodiscard]] bool snap() const { return snap_; }
    [[nodiscard]] int drag_edge() const { return drag_edge_; }
    [[nodiscard]] int64_t duration() const { return dur_; }
    [[nodiscard]] int64_t left() const { return left_; }
    [[nodiscard]] int64_t right() const { return right_; }
    [[nodiscard]] const CutTarget& target() const { return t_; }

private:
    CutTarget t_;
    bool visible_ = false;
    bool dragging_ = false;
    int drag_edge_ = kDragEdgeNone;
    bool snap_ = false;
    int64_t anchor_ = 0;   // the fixed edge while a resize edge drags
    int64_t dur_ = 0;      // active/final duration in frames
    int64_t left_ = 0;     // overlay span across the boundary
    int64_t right_ = 0;
};

}  // namespace transition_editor
}  // namespace canvas::gui