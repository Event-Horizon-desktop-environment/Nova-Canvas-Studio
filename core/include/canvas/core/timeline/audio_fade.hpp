#pragma once

#include "canvas/core/timeline/model.hpp"

#include <cmath>

namespace canvas::core {

namespace detail {
constexpr float kAudioFadePi = 3.14159265358979323846f;
}

[[nodiscard]] inline float audio_fade_gain(const Clip& clip, const int64_t tl_frame) {
    float gain = 1.0f;

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

}
