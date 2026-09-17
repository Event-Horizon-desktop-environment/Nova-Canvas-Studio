#pragma once

#include "canvas/core/timeline/model.hpp"

#include <array>
#include <cstdint>
#include <map>
#include <vector>

namespace canvas::core {

class ParametricEqualizer final {
public:
    struct Coeff {
        double b0, b1, b2, a1, a2;
    };

    [[nodiscard]] bool configure(int sample_rate, int channels,
                                 const std::array<Clip::EqBand, 6>& bands);

    [[nodiscard]] bool active() const;

    void process(float* samples, int num_frames);

    void reset();

    static constexpr int kGlideFrames = 128;

private:
    std::vector<Coeff> coeffs_;
    int channels_ = 0;
    std::vector<float> z1_;
    std::vector<float> z2_;
    std::vector<Coeff> glide_from_;
    std::vector<Coeff> glide_to_;
    int glide_left_ = 0;
    std::array<bool, 6> prev_active_{};
    int prev_channels_ = 1;
    bool any_active_ = false;
};

[[nodiscard]] double equalizer_response(const std::array<Clip::EqBand, 6>& bands,
                                        int sample_rate, double frequency);

[[nodiscard]] double equalizer_band_response(const Clip::EqBand& band, int sample_rate,
                                             double frequency);

class EqualizerBank final {
public:
    [[nodiscard]] int tick(std::uint64_t clip_id, const std::array<Clip::EqBand, 6>& bands,
                           bool enabled, int sample_rate, int channels, float* samples,
                           int num_frames);

    [[nodiscard]] bool wants_samples(std::uint64_t clip_id, bool enabled) const;

    void drop();
    void drop(std::uint64_t clip_id);
    void clear();

private:
    struct Entry {
        ParametricEqualizer eq;
        std::array<Clip::EqBand, 6> bands{};
        int sample_rate = 0;
        int channels = 0;
        bool enabled = false;
        float wet_ = 0.0f;
        std::vector<float> scratch_ = {};
    };
    void glide_to(Entry& entry, float target, float* samples, int channels, int num_frames);

    std::map<std::uint64_t, Entry> entries_;
};

}
