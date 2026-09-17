#include "canvas/core/media/equalizer.hpp"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

using namespace canvas::core;

namespace {

constexpr int kRate = 48000;

int failures = 0;

void check(bool cond, const char* what) {
    if (!cond) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++failures;
    } else {
        std::printf("ok:   %s\n", what);
    }
}

bool near(double a, double b, double tol) { return std::fabs(a - b) <= tol; }

std::array<Clip::EqBand, 6> flat_bands() {
    std::array<Clip::EqBand, 6> bands{};
    for (auto& b : bands) b.type = Clip::EqBand::Type::Bell;
    return bands;
}

std::array<Clip::EqBand, 6> one_band(Clip::EqBand::Type type, float freq, float gain,
                                     float q) {
    std::array<Clip::EqBand, 6> bands = flat_bands();
    bands[0].type = type;
    bands[0].frequency = freq;
    bands[0].gain = gain;
    bands[0].q = q;
    return bands;
}

std::vector<float> sine(double freq, int frames, double amp = 0.5, int channels = 1,
                        int rate = kRate) {
    std::vector<float> out(static_cast<std::size_t>(frames) * channels);
    for (int f = 0; f < frames; ++f) {
        const float v =
            static_cast<float>(amp * std::sin(2.0 * 3.141592653589793 * freq * f / rate));
        for (int c = 0; c < channels; ++c)
            out[static_cast<std::size_t>(f) * channels + c] = v;
    }
    return out;
}

std::vector<float> complex_signal(int frames) {
    std::vector<float> out(static_cast<std::size_t>(frames));
    for (int f = 0; f < frames; ++f) {
        double v = 0.0;
        v += 0.30 * std::sin(2.0 * 3.141592653589793 * 87.0 * f / kRate);
        v += 0.24 * std::sin(2.0 * 3.141592653589793 * 440.0 * f / kRate + 0.7);
        v += 0.21 * std::sin(2.0 * 3.141592653589793 * 1234.0 * f / kRate + 1.9);
        v += 0.17 * std::sin(2.0 * 3.141592653589793 * 6112.5 * f / kRate + 2.6);
        out[static_cast<std::size_t>(f)] = static_cast<float>(v);
    }
    return out;
}

double tail_rms_ratio(const std::vector<float>& in, const std::vector<float>& out,
                      int frames, int channels = 1) {
    assert(in.size() == out.size());
    const int skip = frames / 3;
    double in_ss = 0.0, out_ss = 0.0;
    int n = 0;
    for (int f = skip; f < frames; ++f) {
        for (int c = 0; c < channels; ++c) {
            const std::size_t k = static_cast<std::size_t>(f) * channels + c;
            in_ss += static_cast<double>(in[k]) * in[k];
            out_ss += static_cast<double>(out[k]) * out[k];
            ++n;
        }
    }
    if (n == 0) return 0.0;
    return std::sqrt(out_ss / std::max(in_ss, 1e-12));
}

void test_response_law() {
    {
        const auto bands = one_band(Clip::EqBand::Type::Bell, 1000.0f, 12.0f, 1.0f);
        check(near(equalizer_response(bands, kRate, 1000.0), 12.0, 0.1),
              "bell +12 dB hits +12 at f0");
        check(near(equalizer_response(bands, kRate, 100.0), 0.0, 0.6),
              "bell +12 dB is flat far away");
    }
    {
        const auto ls = one_band(Clip::EqBand::Type::LowShelf, 200.0f, 12.0f, 0.707f);
        check(near(equalizer_response(ls, kRate, 20.0), 12.0, 0.1),
              "low shelf +12 plateau at 20 Hz");
        check(near(equalizer_response(ls, kRate, 12000.0), 0.0, 0.2),
              "low shelf +12 unity at 12 kHz");
        const auto hs = one_band(Clip::EqBand::Type::HighShelf, 4000.0f, -12.0f, 0.707f);
        check(near(equalizer_response(hs, kRate, 20000.0), -12.0, 0.1),
              "high shelf -12 plateau at 20 kHz");
        check(near(equalizer_response(hs, kRate, 100.0), 0.0, 0.2),
              "high shelf -12 unity at 100 Hz");
    }
    {
        const auto lp = one_band(Clip::EqBand::Type::LowPass, 1000.0f, 0.0f, 0.707f);
        check(near(equalizer_response(lp, kRate, 1000.0), -3.01, 0.05),
              "low pass -3 dB at f0");
        check(equalizer_response(lp, kRate, 8000.0) < -20.0, "low pass rolls off hard");
        const auto hp = one_band(Clip::EqBand::Type::HighPass, 1000.0f, 0.0f, 0.707f);
        check(near(equalizer_response(hp, kRate, 1000.0), -3.01, 0.05),
              "high pass -3 dB at f0");
        check(equalizer_response(hp, kRate, 100.0) < -20.0, "high pass rolls off hard");
    }
    {
        const auto notch = one_band(Clip::EqBand::Type::Notch, 1000.0f, 0.0f, 1.0f);
        check(equalizer_response(notch, kRate, 1000.0) < -40.0, "notch nulls f0");
        check(near(equalizer_response(notch, kRate, 200.0), 0.0, 0.5) &&
                  near(equalizer_response(notch, kRate, 5000.0), 0.0, 0.5),
              "notch unity off-band");
    }
    {
        const auto flat = flat_bands();
        check(near(equalizer_response(flat, kRate, 20.0), 0.0, 1e-9) &&
                  near(equalizer_response(flat, kRate, 1000.0), 0.0, 1e-9) &&
                  near(equalizer_response(flat, kRate, 19000.0), 0.0, 1e-9),
              "flat EQ is 0 dB everywhere");
    }
    {
        auto bands = one_band(Clip::EqBand::Type::Bell, 1000.0f, 12.0f, 1.0f);
        bands[1] = bands[0];
        bands[1].frequency = 4000.0f;
        for (const double hz : {20.0, 100.0, 1000.0, 4000.0, 19000.0}) {
            const double per_band = equalizer_band_response(bands[0], kRate, hz) +
                                    equalizer_band_response(bands[1], kRate, hz);
            check(near(per_band, equalizer_response(bands, kRate, hz), 1e-9),
                  "composite == sum of per-band responses at f0-spaced bells");
        }
        const double before = equalizer_band_response(bands[1], kRate, 4000.0);
        bands[1].enabled = false;
        check(near(equalizer_band_response(bands[1], kRate, 4000.0), 0.0, 1e-9),
              "disabled band contributes 0 dB despite its settings");
        bands[1].enabled = true;
        check(near(equalizer_band_response(bands[1], kRate, 4000.0), before, 1e-9),
              "re-enabled band restores its per-band response");
    }
}

void test_process_law() {
    {
        const int frames = 24000;
        const auto in = sine(1000.0, frames);
        std::vector<float> out = in;
        ParametricEqualizer eq;
        check(eq.configure(kRate, 1, one_band(Clip::EqBand::Type::Bell, 1000.0f, 12.0f, 1.0f)),
              "configure with an active band returns active");
        eq.process(out.data(), frames);
        check(near(tail_rms_ratio(in, out, frames), 3.981, 0.08),
              "bell +12 dB boosts a 1 kHz tone by 3.98x");
    }
    {
        const int frames = 24000;
        const auto in = sine(100.0, frames);
        std::vector<float> out = in;
        ParametricEqualizer eq;
        (void)eq.configure(kRate, 1, one_band(Clip::EqBand::Type::Bell, 1000.0f, 12.0f, 1.0f));
        eq.process(out.data(), frames);
        check(near(tail_rms_ratio(in, out, frames), 1.0, 0.02),
              "bell +12 dB leaves a 100 Hz tone untouched");
    }
    {
        const int frames = 24000;
        const auto bands = one_band(Clip::EqBand::Type::LowPass, 200.0f, 0.0f, 0.707f);
        ParametricEqualizer eq;
        (void)eq.configure(kRate, 1, bands);
        auto lo_in = sine(100.0, frames);
        auto lo_out = lo_in;
        eq.process(lo_out.data(), frames);
        check(near(tail_rms_ratio(lo_in, lo_out, frames), 1.0, 0.03),
              "low pass @200 passes 100 Hz");
        eq.reset();
        auto hi_in = sine(5000.0, frames);
        auto hi_out = hi_in;
        eq.process(hi_out.data(), frames);
        check(tail_rms_ratio(hi_in, hi_out, frames) < 0.02, "low pass @200 kills 5 kHz");
    }
    {
        const int frames = 24000;
        const auto bands = one_band(Clip::EqBand::Type::HighPass, 200.0f, 0.0f, 0.707f);
        ParametricEqualizer eq;
        (void)eq.configure(kRate, 1, bands);
        auto hi_in = sine(5000.0, frames);
        auto hi_out = hi_in;
        eq.process(hi_out.data(), frames);
        check(near(tail_rms_ratio(hi_in, hi_out, frames), 1.0, 0.03),
              "high pass @200 passes 5 kHz");
        eq.reset();
        auto lo_in = sine(100.0, frames);
        auto lo_out = lo_in;
        eq.process(lo_out.data(), frames);
        const double hp_ratio = tail_rms_ratio(lo_in, lo_out, frames);
        check(hp_ratio < 0.5 && hp_ratio > 0.1, "high pass @200 rolls off 100 Hz by ~-13 dB");
    }
    {
        const int frames = 24000;
        const auto bands = one_band(Clip::EqBand::Type::Notch, 1000.0f, 0.0f, 1.0f);
        ParametricEqualizer eq;
        (void)eq.configure(kRate, 1, bands);
        auto n_in = sine(1000.0, frames);
        auto n_out = n_in;
        eq.process(n_out.data(), frames);
        check(tail_rms_ratio(n_in, n_out, frames) < 0.02, "notch nulls a 1 kHz tone");
        eq.reset();
        auto o_in = sine(200.0, frames);
        auto o_out = o_in;
        eq.process(o_out.data(), frames);
        check(near(tail_rms_ratio(o_in, o_out, frames), 1.0, 0.05),
              "notch leaves 200 Hz untouched");
    }
    {
        const int frames = 8000;
        auto in = sine(440.0, frames);
        std::vector<float> out = in;
        ParametricEqualizer eq;
        check(!eq.configure(kRate, 1, flat_bands()), "flat EQ configures to inactive");
        eq.process(out.data(), frames);
        check(out == in, "flat EQ process is bit-exact pass-through");
    }
}

void test_streaming_equivalence() {
    const int frames = 60000;
    const auto in = complex_signal(frames);
    const auto bands = one_band(Clip::EqBand::Type::Bell, 900.0f, 8.0f, 1.4f);

    std::vector<float> whole = in;
    ParametricEqualizer whole_eq;
    (void)whole_eq.configure(kRate, 1, bands);
    whole_eq.process(whole.data(), frames);

    std::vector<float> chunked = in;
    ParametricEqualizer chunk_eq;
    (void)chunk_eq.configure(kRate, 1, bands);
    constexpr int kChunk = 797;
    for (int off = 0; off < frames; off += kChunk) {
        const int n = std::min(kChunk, frames - off);
        chunk_eq.process(chunked.data() + off, n);
    }
    float worst = 0.0f;
    for (int i = 0; i < frames; ++i)
        worst = std::max(worst, std::fabs(whole[static_cast<std::size_t>(i)] -
                                          chunked[static_cast<std::size_t>(i)]));
    check(worst < 1e-5f, "chunked EQ processing equals whole-buffer processing");
}

void test_channel_stereo() {
    const int frames = 24000;
    const int ch = 2;
    const auto bands = one_band(Clip::EqBand::Type::LowPass, 200.0f, 0.0f, 0.707f);
    std::vector<float> stereo(static_cast<std::size_t>(frames) * ch);
    for (int f = 0; f < frames; ++f) {
        const double t_l = 2.0 * 3.141592653589793 * 100.0 * f / kRate;
        const double t_r = 2.0 * 3.141592653589793 * 5000.0 * f / kRate;
        stereo[static_cast<std::size_t>(f) * 2 + 0] = static_cast<float>(0.4 * std::sin(t_l));
        stereo[static_cast<std::size_t>(f) * 2 + 1] = static_cast<float>(0.4 * std::sin(t_r));
    }
    const auto in = stereo;
    ParametricEqualizer eq;
    (void)eq.configure(kRate, ch, bands);
    eq.process(stereo.data(), frames);

    double l_in = 0.0, l_out = 0.0, r_in = 0.0, r_out = 0.0;
    int n = 0;
    for (int f = frames / 3; f < frames; ++f) {
        const double li = in[static_cast<std::size_t>(f) * 2 + 0];
        const double ri = in[static_cast<std::size_t>(f) * 2 + 1];
        const double lo = stereo[static_cast<std::size_t>(f) * 2 + 0];
        const double ro = stereo[static_cast<std::size_t>(f) * 2 + 1];
        l_in += li * li; l_out += lo * lo; r_in += ri * ri; r_out += ro * ro;
        ++n;
    }
    check(n > 0 && std::sqrt(l_out / l_in) > 0.9, "stereo L channel (100 Hz) passes the low pass");
    check(n > 0 && std::sqrt(r_out / r_in) < 0.05, "stereo R channel (5 kHz) is killed by the low pass");
}

void test_sample_rate_independence() {
    for (const int rate : {44100, 48000, 96000}) {
        const auto bands = one_band(Clip::EqBand::Type::Bell, 1000.0f, 12.0f, 1.0f);
        check(near(equalizer_response(bands, rate, 1000.0), 12.0, 0.1),
              "bell +12 dB holds at every sample rate");
        const auto lp = one_band(Clip::EqBand::Type::LowPass, 1000.0f, 0.0f, 0.707f);
        check(near(equalizer_response(lp, rate, 1000.0), -3.01, 0.05),
              "low pass -3 dB holds at every sample rate");
    }
}

void test_band_enabled() {
    {
        auto bands = one_band(Clip::EqBand::Type::Bell, 1000.0f, 12.0f, 1.0f);
        check(equalizer_response(bands, kRate, 1000.0) > 11.0, "enabled band shapes response");
        bands[0].enabled = false;
        check(near(equalizer_response(bands, kRate, 1000.0), 0.0, 1e-9),
              "disabled band contributes nothing to the response");
    }
    {
        const int frames = 24000;
        auto bands = one_band(Clip::EqBand::Type::Bell, 1000.0f, 12.0f, 1.0f);
        bands[0].enabled = false;
        auto in = sine(1000.0, frames);
        auto out = in;
        ParametricEqualizer eq;
        check(!eq.configure(kRate, 1, bands), "all-disabled EQ configures to inactive");
        eq.process(out.data(), frames);
        check(out == in, "all-disabled EQ process is bit-exact pass-through");
    }
    {
        const int frames = 24000;
        auto bands = one_band(Clip::EqBand::Type::Bell, 1000.0f, 12.0f, 1.0f);
        bands[1] = bands[0];
        bands[1].frequency = 3000.0f;
        auto in = sine(1000.0, frames);
        auto out = in;
        ParametricEqualizer eq;
        (void)eq.configure(kRate, 1, bands);
        bands[1].enabled = false;
        (void)eq.configure(kRate, 1, bands);
        eq.process(out.data(), frames);
        check(near(tail_rms_ratio(in, out, frames), 3.981, 0.08),
              "disabling one band keeps the other bands' law");
    }
    {
        const int kBankFrames = 24000;
        EqualizerBank bank;
        auto bands = one_band(Clip::EqBand::Type::Notch, 1000.0f, 0.0f, 1.0f);
        auto in = sine(1000.0, kBankFrames);
        auto out = in;
        bands[0].enabled = false;
        (void)bank.tick(4, bands, true, kRate, 1, out.data(), kBankFrames);
        check(tail_rms_ratio(in, out, kBankFrames) > 0.99,
              "bank with a single disabled band passes through");
        bands[0].enabled = true;
        auto out2 = in;
        (void)bank.tick(4, bands, true, kRate, 1, out2.data(), kBankFrames);
        check(tail_rms_ratio(in, out2, kBankFrames) < 0.02,
              "re-enabling the band restores the notch law");
    }
}

void test_bank() {
    const int frames = 24000;

    {
        const auto boost = one_band(Clip::EqBand::Type::Bell, 1000.0f, 12.0f, 1.0f);
        const auto cut = one_band(Clip::EqBand::Type::Bell, 1000.0f, -12.0f, 1.0f);
        EqualizerBank bank;
        auto a_in = sine(1000.0, frames);
        auto a_out = a_in;
        auto b_in = sine(1000.0, frames);
        auto b_out = b_in;
        (void)bank.tick(1, boost, true, kRate, 1, a_out.data(), frames);
        (void)bank.tick(2, cut, true, kRate, 1, b_out.data(), frames);
        check(near(tail_rms_ratio(a_in, a_out, frames), 3.981, 0.08),
              "clip A bell +12 dB boosts");
        check(near(tail_rms_ratio(b_in, b_out, frames), 0.251, 0.015),
              "clip B bell -12 dB cuts");
    }

    {
        EqualizerBank bank;
        const auto bands = one_band(Clip::EqBand::Type::Bell, 1000.0f, 12.0f, 1.0f);
        const int seg = 5000;
        const auto whole = sine(1000.0, 3 * seg);

        auto first = std::vector<float>(whole.begin(), whole.begin() + seg);
        auto fout = first;
        (void)bank.tick(7, bands, true, kRate, 1, fout.data(), seg);
        auto gapped = std::vector<float>(whole.begin() + seg, whole.begin() + 2 * seg);
        auto gout = gapped;
        (void)bank.tick(7, bands, false, kRate, 1, gout.data(), seg);
        const float disable_edge = std::fabs(static_cast<float>(gout[0]) -
                                             static_cast<float>(fout[seg - 1]));
        check(disable_edge < 0.4f, "disable edge crossfades (no one-sample boom)");
        bool dry_exact = true;
        for (int i = ParametricEqualizer::kGlideFrames; i < seg; ++i) {
            if (gout[static_cast<std::size_t>(i)] != gapped[static_cast<std::size_t>(i)]) {
                dry_exact = false;
                break;
            }
        }
        check(dry_exact,
              "disabled region is byte-exact pass-through after the crossfade window");
        auto cont_in = std::vector<float>(whole.begin() + 2 * seg, whole.end());
        auto carried = cont_in;
        (void)bank.tick(7, bands, true, kRate, 1, carried.data(), seg);
        const float enable_edge = std::fabs(static_cast<float>(carried[0]) -
                                            static_cast<float>(gout[seg - 1]));
        check(enable_edge < 0.4f, "re-enable edge crossfades (no one-sample boom)");
        ParametricEqualizer fresh;
        (void)fresh.configure(kRate, 1, bands);
        auto ref = cont_in;
        fresh.process(ref.data(), seg);
        float worst_vs_fresh = 0.0f;
        for (int i = 0; i < seg; ++i)
            worst_vs_fresh =
                std::max(worst_vs_fresh, std::fabs(carried[static_cast<std::size_t>(i)] -
                                                   ref[static_cast<std::size_t>(i)]));
        check(worst_vs_fresh > 1e-4f,
              "re-enable resumes carried state, not a cold start (glide present)");
        check(near(tail_rms_ratio(cont_in, carried, seg), 3.981, 0.08),
              "re-enable restores the +12 dB bell law (steady state)");
        check(!near(static_cast<double>(fout[200]), static_cast<double>(first[200]), 1e-6),
              "pre-disable run was actually filtered");
    }

    {
        EqualizerBank bank;
        const auto boost = one_band(Clip::EqBand::Type::Bell, 1000.0f, 12.0f, 1.0f);
        const int seg = 8000;
        auto data = sine(1000.0, seg);
        (void)bank.tick(9, boost, true, kRate, 1, data.data(), seg / 2);
        std::array<Clip::EqBand, 6> new_bands{};
        for (auto& b : new_bands) b.type = Clip::EqBand::Type::Bell;
        auto tail_in = sine(1000.0, seg / 2);
        auto tail_out = tail_in;
        (void)bank.tick(9, new_bands, true, kRate, 1, tail_out.data(), seg / 2);
        check(tail_out == tail_in, "band edit to flat is bit-exact after reconfig");
    }

    {
        EqualizerBank bank;
        const int seg = 8000;
        const auto boost_12 = one_band(Clip::EqBand::Type::Bell, 1000.0f, 12.0f, 1.0f);
        const auto boost_14 = one_band(Clip::EqBand::Type::Bell, 1000.0f, 14.0f, 1.0f);
        const auto in = sine(1000.0, seg);
        const int boundary = seg / 2 - 64;
        std::vector<float> carried = in;
        (void)bank.tick(10, boost_12, true, kRate, 1, carried.data(), boundary);
        (void)bank.tick(10, boost_14, true, kRate, 1, carried.data() + boundary,
                        seg - boundary);

        auto ref12 = in;
        ParametricEqualizer eq12;
        (void)eq12.configure(kRate, 1, boost_12);
        eq12.process(ref12.data(), seg);

        check(std::fabs(carried[boundary] - ref12[boundary]) < 1e-5f,
              "mid-stream gain drag: boundary sample equals the +12 continuation");
        float drag_drift = 0.0f;
        for (int i = boundary; i < boundary + 16; ++i)
            drag_drift =
                std::max(drag_drift, std::fabs(carried[static_cast<std::size_t>(i)] -
                                               ref12[static_cast<std::size_t>(i)]));
        check(drag_drift < 0.05f,
              "mid-stream gain drag stays near the old curve (no cold-start divergence)");
        std::vector<float> tail_in(in.begin() + boundary + 512, in.end());
        std::vector<float> tail_out(carried.begin() + boundary + 512, carried.end());
        check(near(tail_rms_ratio(tail_in, tail_out, static_cast<int>(tail_out.size())), 5.012,
                   0.08),
              "mid-stream gain drag converges to the +14 dB law");
    }

    {
        EqualizerBank bank;
        const auto bands = one_band(Clip::EqBand::Type::LowPass, 200.0f, 0.0f, 0.707f);
        auto in = sine(100.0, frames);
        auto out = in;
        (void)bank.tick(3, bands, true, kRate, 1, out.data(), frames / 2);
        bank.drop(3);
        bank.drop();
        const int rest = frames - frames / 2;
        (void)bank.tick(3, bands, true, kRate, 1, out.data() + frames / 2, rest);
        check(near(tail_rms_ratio(in, out, frames), 1.0, 0.04),
              "bank drop() then continue keeps the low-pass law");
    }

    {
        EqualizerBank bank;
        const auto bands = one_band(Clip::EqBand::Type::HighPass, 800.0f, 0.0f, 3.0f);
        const int seg = 4000;
        auto make_sig = [&] {
            std::vector<float> s(seg);
            for (int f = 0; f < seg; ++f)
                s[f] = static_cast<float>(0.7 * std::sin(2.0 * 3.141592653589793 * 120.0 * f /
                                                          kRate + 1.7));
            return s;
        };

        auto first = make_sig();
        (void)bank.tick(20, bands, true, kRate, 1, first.data(), seg);

        bank.drop();

        auto raw = make_sig();
        auto resumed = raw;
        (void)bank.tick(20, bands, true, kRate, 1, resumed.data(), seg);

        check(near(static_cast<double>(resumed[0]), static_cast<double>(raw[0]), 1e-5),
              "seek+resume on an enabled clip starts fully dry (re-glides, no exposed cold start)");
        float worst_over = 0.0f;
        for (int i = 0; i < ParametricEqualizer::kGlideFrames; ++i)
            worst_over = std::max(
                worst_over, std::fabs(resumed[static_cast<std::size_t>(i)]) - std::fabs(raw[static_cast<std::size_t>(i)]));
        check(worst_over < 0.15f,
              "seek+resume glide never overshoots the raw signal by more than a small, "
              "properly-glided residual (no exposed cold-start pop)");
        std::vector<float> tail_raw(raw.begin() + 1024, raw.end());
        std::vector<float> tail_out(resumed.begin() + 1024, resumed.end());
        check(tail_rms_ratio(tail_raw, tail_out, static_cast<int>(tail_out.size())) < 0.05,
              "seek+resume still converges to the high-pass law after the glide");
    }

    {
        EqualizerBank bank;
        const auto bands = one_band(Clip::EqBand::Type::Notch, 1000.0f, 0.0f, 1.0f);
        auto in = sine(1000.0, frames, 0.5, 1, 44100);
        auto out = in;
        (void)bank.tick(5, bands, true, 44100, 1, out.data(), frames);
        check(tail_rms_ratio(in, out, frames) < 0.02,
              "bank runs the notch at 44.1 kHz too");
    }

    {
        EqualizerBank bank;
        const auto bands = one_band(Clip::EqBand::Type::Bell, 1000.0f, 12.0f, 1.0f);
        const int seg = 6000;

        auto st_in = sine(1000.0, seg, 0.5, 2);
        auto st_out = st_in;
        (void)bank.tick(11, bands, true, kRate, 2, st_out.data(), seg);
        check(near(tail_rms_ratio(st_in, st_out, seg, 2), 3.981, 0.08),
              "stereo tick before channel flip boosts");
        check(std::isfinite(static_cast<double>(st_out[seg * 2 - 1])),
              "stereo tick output is finite");

        auto mo_in = sine(1000.0, seg);
        auto mo_out = mo_in;
        (void)bank.tick(11, bands, true, kRate, 1, mo_out.data(), seg);
        check(near(tail_rms_ratio(mo_in, mo_out, seg), 3.981, 0.08),
              "mono tick after stereo config reconfigures (no stale stride)");
        {
            ParametricEqualizer fresh;
            (void)fresh.configure(kRate, 1, bands);
            auto ref = mo_in;
            fresh.process(ref.data(), seg);
            float worst = 0.0f;
            for (int i = seg / 2; i < seg; ++i)
                worst = std::max(worst, std::fabs(mo_out[static_cast<std::size_t>(i)] -
                                                  ref[static_cast<std::size_t>(i)]));
            check(worst < 0.01f,
                  "mono-after-stereo converges to a fresh mono filter (state carry)");
        }

        auto st2_in = sine(1000.0, seg, 0.5, 2);
        auto st2_out = st2_in;
        (void)bank.tick(11, bands, true, kRate, 2, st2_out.data(), seg);
        check(near(tail_rms_ratio(st2_in, st2_out, seg, 2), 3.981, 0.08),
              "stereo-after-mono reconfigures back to 2ch");
    }
}

}

int main() {
    test_response_law();
    test_process_law();
    test_band_enabled();
    test_streaming_equivalence();
    test_channel_stereo();
    test_sample_rate_independence();
    test_bank();

    std::printf("equalizer: %d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
