#pragma once

// TimelineDragController — Qt-free clip-drag math + session state for the
// timeline (splitplan Phase 29). The widget keeps the QGraphicsView plumbing
// (mouse events, mapToScene, track_at_y hit-testing, filmstrip painting, snap
// indicator, signals) and delegates the decision logic here so it can be
// unit-tested from the headless seam (gui/tests) and scanned by
// scripts/check_qtdep.sh.
//
// The controller owns exactly the state a clip drag transition needs from press
// to release:
//   * grab_frames_  — the pointer's frame offset into the clip at press, frozen
//     so the clip cannot "jump" under the cursor while it follows the pointer;
//   * original_track_index_ — the flat track the press grabbed on, used by the
//     release commit test (did the clip change time AND/OR track?);
//   * target_track_index_   — the last VALID same-kind track under the pointer,
//     re-resolved every move and kept when the cursor leaves a drop zone (the
//     clip visually stays on its last valid track, matching pre-extraction
//     behaviour where the widget only mutated the track while over a same-kind
//     channel).
//
// All frame math is clamped >= 0; snapping is delegated to the timeline_snap
// module (the widget passes its effective "snap is on" predicate + the current
// zoom). No Qt headers.

#include <algorithm>
#include <cstdint>

#include "canvas/core/timeline/model.hpp"
#include "Widgets/timeline_snap.hpp"

namespace canvas::gui {
namespace timeline_drag {

class DragController {
public:
    // Start a clip drag at the pointer's press frame, remembering how far into
    // the clip the grab happened and which flat track it started on.
    void begin(int64_t pointer_frame_at_press, int64_t clip_tl_in,
               int original_flat_track);
    void end() { active_ = false; }
    [[nodiscard]] bool active() const { return active_; }

    struct MoveResult {
        int64_t raw_tl_in = 0;  // clamped unsnapped position
        int64_t new_tl_in = 0;  // snapped before returning (when enabled)
        bool snapped = false;   // raw_tl_in != new_tl_in (drives the snap cue)
    };

    // Follow the pointer to a new frame, over a candidate flat track under the
    // cursor (may be -1 = over no track). Crosses clips to another channel of
    // the *same* kind; the last valid target is retained otherwise.
    [[nodiscard]] MoveResult move(int64_t pointer_frame, int candidate_flat_track,
                                  int v_count, canvas::core::Track::Kind current_kind,
                                  bool snap_enabled, double frames_per_pixel);

    struct CommitResult {
        int64_t new_tl_in = 0;
        bool changed = false;  // != press-time tl_in, or target != original track
    };

    // Release: the final snapped position and whether the move actually
    // deviates from the press state (the widget only emits clip_moved then).
    [[nodiscard]] CommitResult commit(int64_t pointer_frame, int64_t current_tl_in,
                                      bool snap_enabled,
                                      double frames_per_pixel) const;

    // The flat track the clip is currently visualised on (last valid target).
    [[nodiscard]] int target_track_index() const { return target_track_index_; }
    [[nodiscard]] int original_track_index() const { return original_track_index_; }

private:
    static int64_t snap_tl_in(int64_t frame, bool snap_enabled,
                              double frames_per_pixel);

    bool active_ = false;
    int64_t grab_frames_ = 0;        // pointer frame - clip tl_in (frozen at press)
    int original_track_index_ = -1;  // flat track at press
    int target_track_index_ = -1;    // last valid same-kind track under the pointer
};

}  // namespace timeline_drag
}  // namespace canvas::gui