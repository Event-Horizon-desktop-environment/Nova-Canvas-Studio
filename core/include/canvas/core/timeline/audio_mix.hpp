#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <vector>

namespace canvas::core {

namespace audio_mix {
constexpr float kMinVolumeDb = -100.0f;
constexpr float kMaxVolumeDb = 24.0f;
constexpr float kVolumeDbSliderMin = -100.0f;
constexpr float kVolumeDbSliderMax = 100.0f;
constexpr float kPanMin = -1.0f;
constexpr float kPanMax = 1.0f;

[[nodiscard]] constexpr float normalize_volume_db(float db) noexcept {
    return std::clamp(db, kMinVolumeDb, kMaxVolumeDb);
}

[[nodiscard]] constexpr float normalize_pan(float pan) noexcept {
    return std::clamp(pan, kPanMin, kPanMax);
}

[[nodiscard]] inline float db_to_gain(float db) noexcept {
    const float v = normalize_volume_db(db);
    if (v <= kMinVolumeDb) return 0.0f;
    return std::pow(10.0f, v / 20.0f);
}

inline void pan_gains(float pan, float& left, float& right) noexcept {
    const float p = normalize_pan(pan);
    if (p <= 0.0f) {
        left = 1.0f;
        right = 1.0f + p;
    } else {
        left = 1.0f - p;
        right = 1.0f;
    }
}

template <typename Container>
[[nodiscard]] inline bool any_solo(const Container& tracks) noexcept {
    for (const auto& t : tracks)
        if (t.solo) return true;
    return false;
}

inline void mix_chunk(std::vector<float>& out, int out_channels,
                      const float* src, int src_ch, int frames,
                      const std::vector<float>* gains, float vol, float gl,
                      float gr) {
    if (out_channels <= 0 || src_ch <= 0 || frames <= 0 || !src || out.empty()) return;
    const std::size_t row = static_cast<std::size_t>(out_channels);
    for (int k = 0; k < frames; ++k) {
        const float g = gains ? (*gains)[static_cast<std::size_t>(k)] : 1.0f;
        const float base = g * vol;
        const std::size_t base_idx = static_cast<std::size_t>(k) * row;
        if (src_ch == 1) {
            const float v = src[static_cast<std::size_t>(k)];
            out[base_idx] += v * base * (out_channels > 1 ? gl : 1.0f);
            if (out_channels > 1) out[base_idx + 1] += v * base * gr;
        } else if (src_ch == 2) {
            const float l = src[static_cast<std::size_t>(k) * 2];
            const float r = src[static_cast<std::size_t>(k) * 2 + 1];
            out[base_idx] += l * base * (out_channels > 1 ? gl : 1.0f);
            if (out_channels > 1) out[base_idx + 1] += r * base * gr;
        } else if (out_channels == 2) {
            const std::size_t so = static_cast<std::size_t>(k) * static_cast<std::size_t>(src_ch);
            out[base_idx] += src[so] * base * gl;
            out[base_idx + 1] += src[so + 1] * base * gr;
            for (int c = 2; c < src_ch; ++c) {
                const float v = src[so + static_cast<std::size_t>(c)] * base * 0.5f;
                if (c % 2 == 0) out[base_idx] += v * gl;
                else out[base_idx + 1] += v * gr;
            }
        } else {
            const std::size_t so = static_cast<std::size_t>(k) * static_cast<std::size_t>(src_ch);
            for (int c = 0; c < src_ch; ++c) {
                const std::size_t dst =
                    c < out_channels ? static_cast<std::size_t>(c) : row - 1;
                out[base_idx + dst] += src[so + static_cast<std::size_t>(c)] * base;
            }
        }
    }
}

inline void mix_chunk(std::vector<float>& out, int out_channels,
                      const std::vector<float>& src, int src_ch,
                      const std::vector<float>* gains, float vol, float gl,
                      float gr) {
    if (out_channels <= 0 || src_ch <= 0 || src.empty() || out.empty()) return;
    mix_chunk(out, out_channels, src.data(), src_ch,
              static_cast<int>(src.size()) / src_ch, gains, vol, gl, gr);
}

}

}
