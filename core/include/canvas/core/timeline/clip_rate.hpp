#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

#include "canvas/core/timeline/audio_processing.hpp"
#include "canvas/core/timeline/model.hpp"

namespace canvas::core::cliprate {

inline double effective_rate(const Clip& clip) {
    if (!clip.speed_enabled) return 1.0;
    return std::clamp(static_cast<double>(clip.speed_factor),
                      static_cast<double>(audio_processing::kSpeedMin),
                      static_cast<double>(audio_processing::kSpeedMax));
}

inline int64_t scaled_frame_offset(const Clip& clip, int64_t tl_offset) {
    return static_cast<int64_t>(std::llround(static_cast<double>(tl_offset) *
                                             effective_rate(clip)));
}

inline int64_t media_span_for_output(const Clip& clip, int64_t out_frames) {
    return static_cast<int64_t>(std::llround(static_cast<double>(out_frames) *
                                             effective_rate(clip)));
}

inline int64_t output_frames_from_media(const Clip& clip, int64_t in_frames) {
    const double r = effective_rate(clip);
    return r <= 0.0 ? 0 : static_cast<int64_t>(std::llround(
                              static_cast<double>(in_frames) / r));
}

inline double pitch_factor(float semitones, float cents) {
    const double st = std::clamp(static_cast<double>(semitones),
                                 static_cast<double>(audio_processing::kPitchSemitonesMin),
                                 static_cast<double>(audio_processing::kPitchSemitonesMax));
    const double ct = std::clamp(static_cast<double>(cents),
                                 static_cast<double>(audio_processing::kPitchCentsMin),
                                 static_cast<double>(audio_processing::kPitchCentsMax));
    return std::pow(2.0, (st + ct / 100.0) / 12.0);
}

inline double pitch_factor(const Clip& clip) {
    return pitch_factor(clip.pitch_semitones, clip.pitch_cents);
}

}
