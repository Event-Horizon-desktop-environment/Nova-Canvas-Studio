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
#include <span>

namespace canvas::gui {
namespace timeline_snap {

// The grid step (in frames) for a given pixels-per-frame zoom level. Returns 1
// (frame-granular) for non-positive or tiny zoom values.
int64_t grid_step(double frames_per_pixel);

// Snap @p frame to the nearest multiple of grid_step(frames_per_pixel). Non-finite
// or non-positive zoom passes the frame through unchanged.
int64_t snap_to_grid(int64_t frame, double frames_per_pixel);

// --- Resolve-style MAGNETIC edge snapping (Phase 8) ---------------------------
//
// DaVinci Resolve's "magnet" does not snap to an invisible grid: clip in/out
// points, the playhead, and markers all magnetise toward lining up with one
// another. The grid below is retained as the zoomed-out fallback; with snapping
// on, the nearest boundary within a pixel-derived radius wins FIRST.

// Magnetic radius in screen pixels, converted to a frame delta by the callers
// (radius * frames_per_pixel — frames-per-pixel, so the magnet matches the
// on-screen pixel size at every zoom; a pre-fix divide made it a ~1-frame dead
// zone at mid/zoomed-out zoom). Edges pull only while their target is within
// this many pixels, so zooming in gives a wide magnet and zooming out naturally
// disables it — matching NLE feel at every zoom level.
inline constexpr double kSnapRadiusPx = 10.0;

// Nearest target in `targets` (must be sorted ascending; a span of the other
// clips' in/out edges, plus the playhead and bookmarks) whose frame falls
// within `max_delta` frames of `p`; returns `p` unchanged when nothing is in
// range. Ties resolve to the nearer one (strictly-smaller delta wins).
int64_t snap_frame_to_edges(int64_t p, int64_t max_delta,
                            std::span<const int64_t> targets);

// Drag-session edge magnetism: BOTH edges of the dragged clip (leading =
// raw_tl_in, trailing = raw_tl_in + duration) compete for the nearest target in
// the sorted `targets` span; the closer pull wins and the clip is positioned so
// that edge lands exactly on the target. `frames_per_pixel` converts
// kSnapRadiusPx into the frame threshold (non-positive disables). Returns the
// new tl_in, or `raw_tl_in` unchanged when neither edge is within range (the
// caller may then fall back to grid snapping). Negative results clamp to 0 so a
// trailing-edge snap can never drag a clip off the timeline's head.
int64_t snap_dragged_edges(int64_t raw_tl_in, int64_t duration,
                           std::span<const int64_t> targets,
                           double frames_per_pixel);

}  // namespace timeline_snap
}  // namespace canvas::gui