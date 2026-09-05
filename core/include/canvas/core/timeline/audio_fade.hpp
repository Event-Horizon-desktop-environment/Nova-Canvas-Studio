#pragma once

// Audio-transition gain envelopes (Qt-free, core domain). Renders the audible
// effect of AUDIO transitions: fade-in ramp over a clip's IN window and fade-out
// ramp over its OUT window. Video-only transition types are ignored, exactly as
// the video renderer ignores audio types — so a mixed-kind transition that was
// stored on the wrong domain (older projects, unlinked clips) stays inert
// instead of introducing an accidental level drop.
//
// The envelopes are single-clip ramps (no two-clip overlap mix): the timeline
// places clips adjacent on a track, so a fade-out reaches ~0 just before the
// cut and the incoming clip fades in from ~0 after it. This matches the
// per-clip transition model; a true equal-power crossfade would additionally
// need the outgoing clip's tail to overlap the incoming clip's head.

#include "canvas/core/timeline/model.hpp"

#include <cmath>

namespace canvas::core {

namespace detail {
constexpr float kAudioFadePi = 3.14159265358979323846f;
}  // namespace detail

// Combined audio gain (0..1) for `clip`'s own audio at timeline frame
// `tl_frame`, from any active AUDIO IN/OUT transitions.
[[nodiscard]] inline float audio_fade_gain(const Clip& clip, const int64_t tl_frame) {
    float gain = 1.0f;

    // Fade-in over [tl_in, tl_in + transition_in_duration).
    if (clip.transition_in_duration > 0 && is_audio_transition(clip.transition_in) &&
        tl_frame >= clip.tl_in && tl_frame < clip.tl_in + clip.transition_in_duration) {
        const float p = static_cast<float>(tl_frame - clip.tl_in) /
                        static_cast<float>(clip.transition_in_duration);
        switch (clip.transition_in) {
            case TransitionType::AudioFadeConstantGain:
                gain *= p;
                break;
            case TransitionType::AudioFadeExponential:
                gain *= p * p;
                break;
            case TransitionType::AudioFadeConstantPower:
                gain *= std::sin(p * detail::kAudioFadePi * 0.5f);
                break;
            default:
                break;
        }
    }

    // Fade-out over [tl_out - transition_out_duration, tl_out).
    if (clip.transition_out_duration > 0 && is_audio_transition(clip.transition_out) &&
        tl_frame >= clip.tl_out - clip.transition_out_duration && tl_frame < clip.tl_out) {
        const float p = static_cast<float>(tl_frame - (clip.tl_out - clip.transition_out_duration)) /
                        static_cast<float>(clip.transition_out_duration);
        switch (clip.transition_out) {
            case TransitionType::AudioFadeConstantGain:
                gain *= 1.0f - p;
                break;
            case TransitionType::AudioFadeExponential:
                gain *= (1.0f - p) * (1.0f - p);
                break;
            case TransitionType::AudioFadeConstantPower:
                gain *= std::cos(p * detail::kAudioFadePi * 0.5f);
                break;
            default:
                break;
        }
    }
    return gain;
}

}  // namespace canvas::core