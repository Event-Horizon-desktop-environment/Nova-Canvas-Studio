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

int64_t snap_frame_to_edges(const int64_t p, const int64_t max_delta,
                            const std::span<const int64_t> targets) {
    if (max_delta <= 0 || targets.empty()) return p;
    auto it = std::lower_bound(targets.begin(), targets.end(), p);
    int64_t best = p;
    int64_t best_delta = max_delta;  // strictly within range, forward direction
    bool have_forward = false;       // a forward target was actually taken
    if (it != targets.end()) {
        const int64_t d = *it - p;
        if (d <= best_delta) {
            best = *it;
            best_delta = d;
            have_forward = true;
        }
    }
    if (it != targets.begin()) {
        const auto lo = it - 1;
        const int64_t d = p - *lo;
        // Backward wins on a strictly smaller delta, and on an exact boundary
        // tie when no forward target was taken (radius is inclusive both ways).
        if (d < best_delta || (d == best_delta && !have_forward)) best = *lo;
    }
    return best;
}

int64_t snap_dragged_edges(const int64_t raw_tl_in, const int64_t duration,
                           const std::span<const int64_t> targets,
                           const double frames_per_pixel) {
    if (frames_per_pixel <= 0.0 || duration <= 0 || targets.empty()) return raw_tl_in;
    // frames_per_pixel is frames PER PIXEL, so the 10px magnet is 10*fpp frames
    // wide here (NOT 10/fpp); the radius then stays ~10 screen px at every zoom.
    const int64_t max_delta = std::max<int64_t>(
        1, static_cast<int64_t>(std::llround(kSnapRadiusPx * frames_per_pixel)));
    const int64_t leading_snap = snap_frame_to_edges(raw_tl_in, max_delta, targets);
    const int64_t trailing_edge = raw_tl_in + duration;
    const int64_t trailing_snap = snap_frame_to_edges(trailing_edge, max_delta, targets);
    const bool leading_pulled = leading_snap != raw_tl_in;
    const bool trailing_pulled = trailing_snap != trailing_edge;
    if (leading_pulled && !trailing_pulled) return leading_snap;
    if (trailing_pulled && !leading_pulled)
        return std::max<int64_t>(0, trailing_snap - duration);
    if (leading_pulled && trailing_pulled) {
        // Both edges have a magnet in range: the CLOSER pull wins (the clip is
        // one rigid body, it cannot tear toward two targets); an exact tie
        // prefers the leading edge so an already-anchored head holds.
        const int64_t d_leading = std::abs(leading_snap - raw_tl_in);
        const int64_t d_trailing = std::abs(trailing_snap - trailing_edge);
        if (d_leading <= d_trailing) return leading_snap;
        return std::max<int64_t>(0, trailing_snap - duration);
    }
    return raw_tl_in;
}

}  // namespace timeline_snap
}  // namespace canvas::gui