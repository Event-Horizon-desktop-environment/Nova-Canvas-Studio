#pragma once

// Shared Speed Change law for a clip (Qt-free). `speed_factor`/`speed_enabled`
// retime the WHOLE clip: video and audio consume the source at factor× the
// timeline rate in lockstep, so A/V stays in sync by construction. The
// per-clip position laws in playback (timeline_decoder, audio_pipeline) and
// export (renderer) all route through the helpers here so they can never
// drift from each other.
//
// Model: the clip's timeline span (tl_in/tl_out) is fixed; the factor scales
// how much source a timeline frame consumes. At factor 2 a 100-frame timeline
// placement swallows 200 frames of source (picture and sound both run 2×
// fast) and the source runs dry at tl_in + (src_out - src_in)/factor if that
// is inside the span.
//
// Video simply stride-decodes the source at the factor. Audio keeps PITCH
// identical (the clip sounds like the same recording at a different tempo) —
// the decoded media is WSOLA time-stretched by TimeStretch (time_stretch.hpp)
// before gains/mix, consuming exactly `effective_rate` source frames per
// output frame.

#include <algorithm>
#include <cmath>
#include <cstdint>

#include "canvas/core/timeline/audio_processing.hpp"
#include "canvas/core/timeline/model.hpp"

namespace canvas::core::cliprate {

// The timeline-consumption multiplier for a clip (1.0 when speed is off).
inline double effective_rate(const Clip& clip) {
    if (!clip.speed_enabled) return 1.0;
    return std::clamp(static_cast<double>(clip.speed_factor),
                      static_cast<double>(audio_processing::kSpeedMin),
                      static_cast<double>(audio_processing::kSpeedMax));
}

// A timeline-frame offset scaled by the clip's speed (rounded to whole source
// frames). Valid when `tl_offset` is a frame count >= 0; callers clamp.
inline int64_t scaled_frame_offset(const Clip& clip, int64_t tl_offset) {
    return static_cast<int64_t>(std::llround(static_cast<double>(tl_offset) *
                                             effective_rate(clip)));
}

// Media samples needed to cover `out_frames` output frames at the clip's rate.
inline int64_t media_span_for_output(const Clip& clip, int64_t out_frames) {
    return static_cast<int64_t>(std::llround(static_cast<double>(out_frames) *
                                             effective_rate(clip)));
}

// Output frames delivered by `in_frames` media frames at the clip's rate
// (inverse of media_span_for_output).
inline int64_t output_frames_from_media(const Clip& clip, int64_t in_frames) {
    const double r = effective_rate(clip);
    return r <= 0.0 ? 0 : static_cast<int64_t>(std::llround(
                              static_cast<double>(in_frames) / r));
}

}  // namespace canvas::core::cliprate