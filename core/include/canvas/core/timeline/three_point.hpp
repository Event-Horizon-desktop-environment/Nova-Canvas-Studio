#pragma once

#include "canvas/core/timeline/model.hpp"

#include <cmath>
#include <cstdint>

namespace canvas::core::three_point {

struct Marks {
    int64_t src_in = 0;
    int64_t src_out = 0;
    int64_t tl_in = 0;

    [[nodiscard]] int64_t src_span() const noexcept {
        return src_out > src_in ? src_out - src_in : 0;
    }
};

[[nodiscard]] inline int64_t timeline_duration(const Marks& m, const double seq_fps,
                                               const double media_fps) noexcept {
    const double ratio = seq_fps > 0.0 && media_fps > 0.0 ? seq_fps / media_fps : 1.0;
    return static_cast<int64_t>(
        std::llround(static_cast<double>(m.src_span()) * ratio));
}

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

}
