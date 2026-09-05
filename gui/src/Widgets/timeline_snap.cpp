#include "Widgets/timeline_snap.hpp"

#include <algorithm>
#include <cmath>

namespace canvas::gui {
namespace timeline_snap {

int64_t grid_step(const double frames_per_pixel) {
    if (frames_per_pixel <= 0.0) return 1;
    double step = 1.0;
    while (step * 10.0 / frames_per_pixel < 24.0) step *= 2.0;
    while (step / frames_per_pixel > 24.0) step /= 2.0;
    return std::max<int64_t>(1, static_cast<int64_t>(std::llround(step)));
}

int64_t snap_to_grid(const int64_t frame, const double frames_per_pixel) {
    if (frames_per_pixel <= 0.0) return frame;
    const int64_t s = grid_step(frames_per_pixel);
    return static_cast<int64_t>(std::llround(static_cast<double>(frame) / s)) * s;
}

}  // namespace timeline_snap
}  // namespace canvas::gui