#pragma once

#include <algorithm>
#include <cstdint>
#include <span>

#include "canvas/core/timeline/model.hpp"
#include "Widgets/timeline_snap.hpp"

namespace canvas::gui {
namespace timeline_drag {

[[nodiscard]] inline int64_t clamp_batch_delta(int64_t primary_orig,
                                               int64_t primary_snapped,
                                               int64_t min_set_orig) {
    return std::max(primary_snapped - primary_orig, -min_set_orig);
}

[[nodiscard]] inline int64_t batch_target_tl_in(int64_t primary_orig,
                                                int64_t primary_snapped,
                                                int64_t other_orig,
                                                int64_t min_set_orig) {
    return std::max<int64_t>(0,
                             other_orig + clamp_batch_delta(primary_orig, primary_snapped,
                                                            min_set_orig));
}

class DragController {
public:
    void begin(int64_t pointer_frame_at_press, int64_t clip_tl_in,
               int original_flat_track, int64_t clip_duration = 1);
    void end() { active_ = false; }
    [[nodiscard]] bool active() const { return active_; }

    struct MoveResult {
        int64_t raw_tl_in = 0;
        int64_t new_tl_in = 0;
        bool snapped = false;
    };

    [[nodiscard]] MoveResult move(int64_t pointer_frame, int candidate_flat_track,
                                  int v_count, canvas::core::Track::Kind current_kind,
                                  bool snap_enabled, double frames_per_pixel,
                                  std::span<const int64_t> snap_targets = {});

    struct CommitResult {
        int64_t new_tl_in = 0;
        bool changed = false;
    };

    [[nodiscard]] CommitResult commit(int64_t pointer_frame, int64_t current_tl_in,
                                      bool snap_enabled,
                                      double frames_per_pixel,
                                      std::span<const int64_t> snap_targets = {}) const;

    [[nodiscard]] int target_track_index() const { return target_track_index_; }
    [[nodiscard]] int original_track_index() const { return original_track_index_; }

private:
    static int64_t snap_tl_in(int64_t frame, int64_t duration, bool snap_enabled,
                              double frames_per_pixel,
                              std::span<const int64_t> snap_targets);

    bool active_ = false;
    int64_t grab_frames_ = 0;
    int64_t duration_ = 1;
    int original_track_index_ = -1;
    int target_track_index_ = -1;
};

}
}
