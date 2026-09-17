#pragma once

// 3-point editing LAW (Qt-free). A "three-point edit" sets an in/out pair on the
// SOURCE (the media's own frames) plus a single in-point on the TIMELINE, then
// places the source window there. The placement teeth (Insert = ripple content
// right, Overwrite = replace/split what overlaps) already live in
// `place_clip`/`place_linked_clip`; this header is the one place the source
// window → clip geometry law lives so the GUI's mark-in/mark-out wiring can't
// re-derive it differently.

#include "canvas/core/timeline/model.hpp"

#include <cmath>
#include <cstdint>

namespace canvas::core::three_point {

struct Marks {
    int64_t src_in = 0;
    int64_t src_out = 0;  // exclusive; <= src_in means an empty window
    int64_t tl_in = 0;    // timeline in-point (frames, clamped >= 0 by callers)

    [[nodiscard]] int64_t src_span() const noexcept {
        return src_out > src_in ? src_out - src_in : 0;
    }
};

// Timeline duration of the source window: time-based (`src_span * seq.fps /
// media.fps`), matching place_clip. Frame-for-frame when either rate is unknown.
[[nodiscard]] inline int64_t timeline_duration(const Marks& m, const double seq_fps,
                                               const double media_fps) noexcept {
    const double ratio = seq_fps > 0.0 && media_fps > 0.0 ? seq_fps / media_fps : 1.0;
    return static_cast<int64_t>(
        std::llround(static_cast<double>(m.src_span()) * ratio));
}

// Build the clip a 3-point edit would place (media id + source window at the
// timeline in-point). An inverted source window normalizes to an empty clip
// (tl_out == tl_in) rather than a negative duration.
[[nodiscard]] inline Clip make_clip(const MediaId media, const Marks& m, const double seq_fps,
                                    const double media_fps) {
    Clip c;
    c.media = media;
    c.src_in = m.src_in;
    c.src_out = m.src_in + m.src_span();
    c.tl_in = m.tl_in;
    c.tl_out = m.tl_in + timeline_duration(m, seq_fps, media_fps);
    return c;
}

}  // namespace canvas::core::three_point