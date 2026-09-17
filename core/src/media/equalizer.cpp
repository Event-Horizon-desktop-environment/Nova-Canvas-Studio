#include "canvas/core/media/equalizer.hpp"
#include "canvas/core/util/log.hpp"

#include <algorithm>
#include <cmath>

#ifndef M_PI
#define M_PI 3.14159265358979323846264338327950288
#endif

namespace canvas::core {

namespace {

constexpr float kIdentityGainEps = 1e-3f;

constexpr double kCascadeNyquistClamp = 0.45;
constexpr double kEvaluateNyquistClamp = 0.499;

double db_amp(const double gain_db) { return std::pow(10.0, gain_db / 40.0); }

ParametricEqualizer::Coeff build_one(const Clip::EqBand::Type type, const double f0,
                                     const double gain_db, const double q,
                                     const int sample_rate) {
    const double Fs = static_cast<double>(sample_rate);
    const double A = db_amp(gain_db);
    const double w0 = 2.0 * M_PI * f0 / Fs;
    const double cw = std::cos(w0);
    const double sw = std::sin(w0);
    const double alpha = sw / (2.0 * std::max(q, 0.05));
    const double sqA = std::sqrt(A);

    double b0, b1, b2, a0, a1, a2;
    switch (type) {
        case Clip::EqBand::Type::LowShelf:
            b0 = A * ((A + 1.0) - (A - 1.0) * cw + 2.0 * sqA * alpha);
            b1 = 2.0 * A * ((A - 1.0) - (A + 1.0) * cw);
            b2 = A * ((A + 1.0) - (A - 1.0) * cw - 2.0 * sqA * alpha);
            a0 = (A + 1.0) + (A - 1.0) * cw + 2.0 * sqA * alpha;
            a1 = -2.0 * ((A - 1.0) + (A + 1.0) * cw);
            a2 = (A + 1.0) + (A - 1.0) * cw - 2.0 * sqA * alpha;
            break;
        case Clip::EqBand::Type::HighShelf:
            b0 = A * ((A + 1.0) + (A - 1.0) * cw + 2.0 * sqA * alpha);
            b1 = -2.0 * A * ((A - 1.0) + (A + 1.0) * cw);
            b2 = A * ((A + 1.0) + (A - 1.0) * cw - 2.0 * sqA * alpha);
            a0 = (A + 1.0) - (A - 1.0) * cw + 2.0 * sqA * alpha;
            a1 = 2.0 * ((A - 1.0) - (A + 1.0) * cw);
            a2 = (A + 1.0) - (A - 1.0) * cw - 2.0 * sqA * alpha;
            break;
        case Clip::EqBand::Type::LowPass:
            b0 = (1.0 - cw) / 2.0;
            b1 = 1.0 - cw;
            b2 = (1.0 - cw) / 2.0;
            a0 = 1.0 + alpha;
            a1 = -2.0 * cw;
            a2 = 1.0 - alpha;
            break;
        case Clip::EqBand::Type::HighPass:
            b0 = (1.0 + cw) / 2.0;
            b1 = -(1.0 + cw);
            b2 = (1.0 + cw) / 2.0;
            a0 = 1.0 + alpha;
            a1 = -2.0 * cw;
            a2 = 1.0 - alpha;
            break;
        case Clip::EqBand::Type::Notch:
            b0 = 1.0;
            b1 = -2.0 * cw;
            b2 = 1.0;
            a0 = 1.0 + alpha;
            a1 = -2.0 * cw;
            a2 = 1.0 - alpha;
            break;
        case Clip::EqBand::Type::Bell:
        default:
            b0 = 1.0 + alpha * A;
            b1 = -2.0 * cw;
            b2 = 1.0 - alpha * A;
            a0 = 1.0 + alpha / A;
            a1 = -2.0 * cw;
            a2 = 1.0 - alpha / A;
            break;
    }

    const double inv = 1.0 / a0;
    return {b0 * inv, b1 * inv, b2 * inv, a1 * inv, a2 * inv};
}

bool band_filters(const Clip::EqBand& b) {
    if (!b.enabled) return false;
    switch (b.type) {
        case Clip::EqBand::Type::Bell:
        case Clip::EqBand::Type::LowShelf:
        case Clip::EqBand::Type::HighShelf:
            return std::fabs(b.gain) > kIdentityGainEps;
        case Clip::EqBand::Type::LowPass:
        case Clip::EqBand::Type::HighPass:
        case Clip::EqBand::Type::Notch:
            return true;
    }
    return false;
}

double clamp_band_f0(const float frequency, const int sample_rate) {
    return std::clamp(static_cast<double>(frequency), 1.0,
                      kCascadeNyquistClamp * static_cast<double>(sample_rate));
}

std::vector<ParametricEqualizer::Coeff> build_cascade(
    const std::array<Clip::EqBand, 6>& bands, const int sample_rate) {
    std::vector<ParametricEqualizer::Coeff> out;
    out.reserve(bands.size());
    if (sample_rate <= 0) return out;
    for (const Clip::EqBand& b : bands) {
        if (!band_filters(b)) continue;
        out.push_back(build_one(b.type, clamp_band_f0(b.frequency, sample_rate), b.gain, b.q,
                                sample_rate));
    }
    return out;
}

ParametricEqualizer::Coeff lerp_coeffs(const ParametricEqualizer::Coeff& from,
                                       const ParametricEqualizer::Coeff& to, const float t) {
    return {from.b0 + (to.b0 - from.b0) * t, from.b1 + (to.b1 - from.b1) * t,
            from.b2 + (to.b2 - from.b2) * t, from.a1 + (to.a1 - from.a1) * t,
            from.a2 + (to.a2 - from.a2) * t};
}

}

bool ParametricEqualizer::configure(const int sample_rate, const int channels,
                                    const std::array<Clip::EqBand, 6>& bands) {
    const std::vector<Coeff> target = build_cascade(bands, sample_rate);
    channels_ = std::max(channels, 1);
    const std::size_t n = target.size() * static_cast<std::size_t>(channels_);
    const std::size_t keep = std::min({z1_.size(), z2_.size(), n});
    std::vector<float> z1_new(n, 0.0f), z2_new(n, 0.0f);
    if (keep > 0) {
        std::copy_n(z1_.begin(), static_cast<std::ptrdiff_t>(keep), z1_new.begin());
        std::copy_n(z2_.begin(), static_cast<std::ptrdiff_t>(keep), z2_new.begin());
    }
    z1_ = std::move(z1_new);
    z2_ = std::move(z2_new);
    if (!coeffs_.empty() && target.size() == coeffs_.size()) {
        glide_from_ = coeffs_;
        glide_to_ = target;
        glide_left_ = kGlideFrames;
    } else {
        coeffs_ = target;
        glide_from_.clear();
        glide_to_.clear();
        glide_left_ = 0;
    }
    return !target.empty();
}

bool ParametricEqualizer::active() const { return !coeffs_.empty(); }

void ParametricEqualizer::process(float* const samples, const int num_frames) {
    if (coeffs_.empty() || samples == nullptr || num_frames <= 0 || channels_ <= 0) return;
    const int ch = channels_;
    const std::size_t stages = coeffs_.size();
    if (stages != glide_to_.size() && glide_left_ > 0) glide_left_ = 0;
    int f = 0;
    while (f < num_frames) {
        if (glide_left_ > 0) {
            const float t = 1.0f - static_cast<float>(glide_left_) / static_cast<float>(kGlideFrames);
            for (std::size_t s = 0; s < stages; ++s)
                coeffs_[s] = lerp_coeffs(glide_from_[s], glide_to_[s], t);
            if (--glide_left_ == 0) {
                coeffs_ = glide_to_;
                glide_from_ = glide_to_;
            }
        }
        const std::size_t base = static_cast<std::size_t>(f) * static_cast<std::size_t>(ch);
        for (int cc = 0; cc < ch; ++cc) {
            std::size_t idx = base + static_cast<std::size_t>(cc);
            float x = samples[idx];
            for (std::size_t s = 0; s < stages; ++s) {
                const Coeff& c = coeffs_[s];
                const std::size_t sz = s * static_cast<std::size_t>(ch);
                float& z1 = z1_[sz + static_cast<std::size_t>(cc)];
                float& z2 = z2_[sz + static_cast<std::size_t>(cc)];
                const float y = static_cast<float>(c.b0 * static_cast<double>(x) + z1);
                z1 = static_cast<float>(c.b1 * static_cast<double>(x) -
                                        c.a1 * static_cast<double>(y) + z2);
                z2 = static_cast<float>(c.b2 * static_cast<double>(x) -
                                        c.a2 * static_cast<double>(y));
                x = y;
            }
            samples[idx] = x;
        }
        ++f;
    }
}

void ParametricEqualizer::reset() {
    std::fill(z1_.begin(), z1_.end(), 0.0f);
    std::fill(z2_.begin(), z2_.end(), 0.0f);
    glide_left_ = 0;
    glide_from_.clear();
    glide_to_.clear();
}

double equalizer_response(const std::array<Clip::EqBand, 6>& bands, const int sample_rate,
                          const double frequency) {
    if (sample_rate <= 0 || frequency <= 0.0) return 0.0;
    const double w = 2.0 * M_PI *
                     std::clamp(frequency, 1.0,
                                kEvaluateNyquistClamp * static_cast<double>(sample_rate)) /
                     static_cast<double>(sample_rate);
    const double cw = std::cos(w);
    const double sw = std::sin(w);
    const double c2 = std::cos(2.0 * w);
    const double s2 = std::sin(2.0 * w);
    double db = 0.0;
    for (const ParametricEqualizer::Coeff& c : build_cascade(bands, sample_rate)) {
        const double br = c.b0 + c.b1 * cw + c.b2 * c2;
        const double bi = -(c.b1 * sw + c.b2 * s2);
        const double ar = 1.0 + c.a1 * cw + c.a2 * c2;
        const double ai = -(c.a1 * sw + c.a2 * s2);
        const double mag2 = (br * br + bi * bi) / (ar * ar + ai * ai);
        if (mag2 > 0.0) db += 10.0 * std::log10(mag2);
    }
    return db;
}

double equalizer_band_response(const Clip::EqBand& band, const int sample_rate,
                               const double frequency) {
    if (sample_rate <= 0 || frequency <= 0.0) return 0.0;
    if (!band_filters(band)) return 0.0;
    const double w = 2.0 * M_PI *
                     std::clamp(frequency, 1.0,
                                kEvaluateNyquistClamp * static_cast<double>(sample_rate)) /
                     static_cast<double>(sample_rate);
    const double cw = std::cos(w);
    const double sw = std::sin(w);
    const double c2 = std::cos(2.0 * w);
    const double s2 = std::sin(2.0 * w);
    const ParametricEqualizer::Coeff c =
        build_one(band.type, clamp_band_f0(band.frequency, sample_rate), band.gain, band.q,
                  sample_rate);
    const double br = c.b0 + c.b1 * cw + c.b2 * c2;
    const double bi = -(c.b1 * sw + c.b2 * s2);
    const double ar = 1.0 + c.a1 * cw + c.a2 * c2;
    const double ai = -(c.a1 * sw + c.a2 * s2);
    const double mag2 = (br * br + bi * bi) / (ar * ar + ai * ai);
    return mag2 > 0.0 ? 10.0 * std::log10(mag2) : 0.0;
}

int EqualizerBank::tick(const std::uint64_t clip_id, const std::array<Clip::EqBand, 6>& bands,
                        const bool enabled, const int sample_rate, const int channels,
                        float* const samples, const int num_frames) {
    if (!enabled) {
        const auto it = entries_.find(clip_id);
        if (it != entries_.end() && it->second.enabled) {
            it->second.enabled = false;
            log::log_audio_info("[eq] disable clip=%llu rate=%d ch=%d (state kept)",
                                static_cast<unsigned long long>(clip_id), it->second.sample_rate,
                                it->second.channels);
        }
        if (it == entries_.end() || it->second.wet_ == 0.0f)
            return num_frames;
        if (!it->second.eq.active()) {
            it->second.wet_ = 0.0f;
            return num_frames;
        }
        if (samples == nullptr || num_frames <= 0) {
            it->second.wet_ = 0.0f;
            return num_frames;
        }
        Entry& entry = it->second;
        entry.scratch_.assign(samples,
                              samples + static_cast<std::size_t>(num_frames) * channels);
        entry.eq.process(samples, num_frames);
        glide_to(entry, 0.0f, samples, channels, num_frames);
        return num_frames;
    }
    Entry& entry = entries_[clip_id];
    const bool fresh = entry.sample_rate == 0 && entry.channels == 0;
    if (entry.bands != bands || entry.sample_rate != sample_rate ||
        entry.channels != channels) {
        log::log_audio_info("[eq] %s clip=%llu rate=%d ch=%d bands=%d",
                            fresh ? "enable" : "reconfigure",
                            static_cast<unsigned long long>(clip_id), sample_rate, channels, 6);
        for (int i = 0; i < 6; ++i)
            log::log_audio_info("[eq]   band[%d] type=%d f=%.0f g=%+.2fdB q=%.2f en=%d", i,
                                static_cast<int>(bands[i].type), bands[i].frequency,
                                bands[i].gain, bands[i].q, bands[i].enabled ? 1 : 0);
        entry.bands = bands;
        entry.sample_rate = sample_rate;
        entry.channels = channels;
        (void)entry.eq.configure(sample_rate, channels, bands);
    } else if (!entry.enabled) {
        log::log_audio_info("[eq] enable clip=%llu rate=%d ch=%d bands=%d (resume from "
                            "carried DF2T state)",
                            static_cast<unsigned long long>(clip_id), sample_rate, channels, 6);
    }
    entry.enabled = true;
    if (samples == nullptr || num_frames <= 0 || !entry.eq.active()) {
        entry.wet_ = 1.0f;
        return num_frames;
    }
    if (entry.wet_ < 1.0f) {
        entry.scratch_.assign(samples,
                              samples + static_cast<std::size_t>(num_frames) * channels);
        entry.eq.process(samples, num_frames);
        glide_to(entry, 1.0f, samples, channels, num_frames);
    } else {
        entry.eq.process(samples, num_frames);
    }
    float peak = 0.0f;
    bool nonfinite = false;
    int bad_at = -1;
    for (int i = 0; i < num_frames * channels; ++i) {
        const float v = samples[i];
        if (std::isnan(v) || std::isinf(v)) {
            nonfinite = true;
            bad_at = i;
            break;
        }
        const float a = std::fabs(v);
        if (a > peak) peak = a;
    }
    if (nonfinite)
        log::log_audio_warning("[eq] NONFINITE clip=%llu at=%d ch=%d rate=%d frames=%d",
                               static_cast<unsigned long long>(clip_id), bad_at, channels,
                               sample_rate, num_frames);
    else if (peak > 32.0f)
        log::log_audio_warning("[eq] PEAK=%.4g clip=%llu ch=%d rate=%d frames=%d",
                               static_cast<double>(peak),
                               static_cast<unsigned long long>(clip_id), channels, sample_rate,
                               num_frames);
    return num_frames;
}

bool EqualizerBank::wants_samples(const std::uint64_t clip_id, const bool enabled) const {
    if (enabled) return true;
    const auto it = entries_.find(clip_id);
    return it != entries_.end() && it->second.wet_ > 0.0f;
}

void EqualizerBank::glide_to(Entry& entry, const float target, float* const samples,
                             const int channels, const int num_frames) {
    const float step = 1.0f / static_cast<float>(ParametricEqualizer::kGlideFrames);
    float wet = entry.wet_;
    for (int f = 0; f < num_frames; ++f) {
        const float w = wet;
        const float dw = 1.0f - w;
        const std::size_t base = static_cast<std::size_t>(f) * channels;
        for (int c = 0; c < channels; ++c) {
            const std::size_t k = base + static_cast<std::size_t>(c);
            samples[k] = w * samples[k] + dw * entry.scratch_[k];
        }
        wet = (w < target) ? std::min(target, w + step) : std::max(target, w - step);
    }
    entry.wet_ = wet;
}

void EqualizerBank::drop() {
    for (auto& kv : entries_) {
        kv.second.eq.reset();
        kv.second.wet_ = 0.0f;
    }
}

void EqualizerBank::drop(const std::uint64_t clip_id) {
    const auto it = entries_.find(clip_id);
    if (it == entries_.end()) return;
    it->second.eq.reset();
    it->second.wet_ = 0.0f;
}

void EqualizerBank::clear() { entries_.clear(); }

}