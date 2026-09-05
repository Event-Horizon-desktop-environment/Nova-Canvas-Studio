// TimelineDragController — Qt-free clip-drag math (splitplan Phase 29).
// See timeline_drag.hpp for the contract and the split boundaries.

#include "Widgets/timeline_drag.hpp"

namespace canvas::gui {
namespace timeline_drag {

void DragController::begin(int64_t pointer_frame_at_press, int64_t clip_tl_in,
                           int original_flat_track) {
    active_ = true;
    grab_frames_ = pointer_frame_at_press - clip_tl_in;
    original_track_index_ = original_flat_track;
    target_track_index_ = original_flat_track;
}

int64_t DragController::snap_tl_in(int64_t frame, bool snap_enabled,
                                   double frames_per_pixel) {
    if (!snap_enabled || frames_per_pixel <= 0.0) return frame;
    return timeline_snap::snap_to_grid(frame, frames_per_pixel);
}

DragController::MoveResult DragController::move(int64_t pointer_frame,
                                                int candidate_flat_track, int v_count,
                                                canvas::core::Track::Kind current_kind,
                                                bool snap_enabled,
                                                double frames_per_pixel) {
    const int64_t raw = std::max<int64_t>(0, pointer_frame - grab_frames_);
    const int64_t next = snap_tl_in(raw, snap_enabled, frames_per_pixel);

    if (candidate_flat_track >= 0) {
        const bool candidate_is_video = candidate_flat_track < v_count;
        const bool current_is_video = current_kind == canvas::core::Track::Kind::Video;
        if (candidate_is_video == current_is_video)
            target_track_index_ = candidate_flat_track;
    }
    return {raw, next, raw != next};
}

DragController::CommitResult DragController::commit(int64_t pointer_frame,
                                                    int64_t current_tl_in,
                                                    bool snap_enabled,
                                                    double frames_per_pixel) const {
    const int64_t new_tl_in =
        snap_tl_in(std::max<int64_t>(0, pointer_frame - grab_frames_), snap_enabled,
                   frames_per_pixel);
    const bool changed =
        new_tl_in != current_tl_in || target_track_index_ != original_track_index_;
    return {new_tl_in, changed};
}

}  // namespace timeline_drag
}  // namespace canvas::gui