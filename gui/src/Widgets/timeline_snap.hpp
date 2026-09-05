#pragma once

// TimelineSnap — pure grid-snapping math for the timeline (splitplan Phase 27).
//
// Qt-free on purpose: this module is exerciseable from the headless test seam
// (gui/tests, linked without Qt) and scanned by scripts/check_qtdep.sh. It never
// includes a <Q...> header. The widget keeps the policy state (snap enabled,
// zoom) in TimelineWidget and delegates the math here so it can be unit-tested.
//
// The snap grid is zoom-dependent: the visual quantizer is the largest
// power-of-two frame count whose on-screen width stays under ~24 px at the given
// frames-per-pixel zoom, so snapped ticks of the playhead/clips stay readable
// while scrubbing.

#include <cstdint>

namespace canvas::gui {
namespace timeline_snap {

// The grid step (in frames) for a given pixels-per-frame zoom level. Returns 1
// (frame-granular) for non-positive or tiny zoom values.
int64_t grid_step(double frames_per_pixel);

// Snap @p frame to the nearest multiple of grid_step(frames_per_pixel). Non-finite
// or non-positive zoom passes the frame through unchanged.
int64_t snap_to_grid(int64_t frame, double frames_per_pixel);

}  // namespace timeline_snap
}  // namespace canvas::gui