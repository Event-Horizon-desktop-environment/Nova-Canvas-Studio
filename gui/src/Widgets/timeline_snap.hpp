#pragma once

#include <cstdint>
#include <span>

namespace canvas::gui {
namespace timeline_snap {

int64_t grid_step(double frames_per_pixel);

int64_t snap_to_grid(int64_t frame, double frames_per_pixel);

inline constexpr double kSnapRadiusPx = 10.0;

int64_t snap_frame_to_edges(int64_t p, int64_t max_delta,
                            std::span<const int64_t> targets);

int64_t snap_dragged_edges(int64_t raw_tl_in, int64_t duration,
                           std::span<const int64_t> targets,
                           double frames_per_pixel);

}
}
