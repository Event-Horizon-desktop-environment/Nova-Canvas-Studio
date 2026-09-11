// Equalizer test: pins the 6-band parametric EQ law (RBJ Audio EQ Cookbook
// biquads via the shared headless seam in equalizer.hpp). Covers the cascade
// magnitude response (equalizer_response), the streaming process() law per
// filter type, chunked-vs-whole streaming equivalence, per-clip bank behavior
// (clip independence, enable toggle state-drop, mid-stream band edits), and
// sample-rate independence (the EQ must run at any playback/export rate).

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

// A pure 6-band array (no default curve baggage).
std::array<Clip::EqBand, 6> flat_bands() {
    std::array<Clip::EqBand, 6> bands{};
    for (auto& b : bands) b.type = Clip::EqBand::Type::Bell;  // gain 0 -> identity
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
    // Deterministic pseudo-random-ish mixture (several incommensurate sines):
    // exercising every band with content instead of a single pure tone.
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

// Steady-state RMS over the tail of a filter run (skips the IIR transient).
double tail_rms_ratio(const std::vector<float>& in, const std::vector<float>& out,
                      int frames, int channels = 1) {
    assert(in.size() == out.size());
    const int skip = frames / 3;  // transient settles well inside the first third
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
    // Both sums share the same sample count, so the RMS ratio is the raw
    // sqrt of the summed-power ratio (no extra normalization needed).
    return std::sqrt(out_ss / std::max(in_ss, 1e-12));
}

void test_response_law() {
    // Bell at f0 is exactly the set gain (RBJ peaking at center = A linear).
    {
        const auto bands = one_band(Clip::EqBand::Type::Bell, 1000.0f, 12.0f, 1.0f);
        check(near(equalizer_response(bands, kRate, 1000.0), 12.0, 0.1),
              "bell +12 dB hits +12 at f0");
        check(near(equalizer_response(bands, kRate, 100.0), 0.0, 0.6),
              "bell +12 dB is flat far away");
    }
    // Shelf: plateau equals the gain at the low end, unity at the far high end.
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
    // Low/High pass: -3.01 dB at the corner.
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
    // Notch: deep null at f0, unity elsewhere.
    {
        const auto notch = one_band(Clip::EqBand::Type::Notch, 1000.0f, 0.0f, 1.0f);
        check(equalizer_response(notch, kRate, 1000.0) < -40.0, "notch nulls f0");
        // RBJ Q=1 notch skirt is a few tenths of a dB at an octave away (measured
        // -0.18 dB @200 Hz, -0.28 dB @5 kHz), so "unity" here means within
        // 0.5 dB, not 0.1.
        check(near(equalizer_response(notch, kRate, 200.0), 0.0, 0.5) &&
                  near(equalizer_response(notch, kRate, 5000.0), 0.0, 0.5),
              "notch unity off-band");
    }
    // Flat EQ (gain-0 bells/shelves, no fixed-shape filters) is exactly 0 dB.
    {
        const auto flat = flat_bands();
        check(near(equalizer_response(flat, kRate, 20.0), 0.0, 1e-9) &&
                  near(equalizer_response(flat, kRate, 1000.0), 0.0, 1e-9) &&
                  near(equalizer_response(flat, kRate, 19000.0), 0.0, 1e-9),
              "flat EQ is 0 dB everywhere");
    }
}

void test_process_law() {
    // Bell +12 dB @1 kHz on a 1 kHz sine: +12 dB = 3.981x steady-state gain.
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
    // Bell +12 dB @1 kHz on a 100 Hz sine: essentially flat.
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
    // Low pass @200 Hz: passes 100 Hz, kills a 5 kHz tone.
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
    // High pass @200 Hz: mirror.
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
        // A 2nd-order RBJ high pass at 200 Hz reaches only -12 dB one octave
        // below the corner (measured ratio 0.224), so "rolls off" means
        // between -6 and -20 dB here, not a near-null.
        const double hp_ratio = tail_rms_ratio(lo_in, lo_out, frames);
        check(hp_ratio < 0.5 && hp_ratio > 0.1, "high pass @200 rolls off 100 Hz by ~-13 dB");
    }
    // Notch @1 kHz: nulls a 1 kHz tone, leaves 200 Hz untouched.
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
    // Flat EQ process is a bit-exact pass-through (no active bands).
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
    // Chunked feeding (odd window sizes, like the real per-step feeds) must be
    // exactly equivalent to one whole-buffer pass: DF2T state flows across
    // chunk boundaries within an Equalizer.
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
    constexpr int kChunk = 797;  // deliberately not a power of two
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
    // Both channels get the same cascade, applied to the correct interleave.
    const int frames = 24000;
    const int ch = 2;
    const auto bands = one_band(Clip::EqBand::Type::LowPass, 200.0f, 0.0f, 0.707f);
    // L = 100 Hz (passes), R = 5 kHz (killed).
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
    // The same band must produce the same law at any rate (playback runs at
    // the pipeline rate; exports can pick 44.1/48/96 kHz).
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
    // Per-band bypass: a disabled band is excluded from the cascade, the
    // response, and the bank — yet keeps its settings (toggling it back on
    // restores the exact band).
    {
        auto bands = one_band(Clip::EqBand::Type::Bell, 1000.0f, 12.0f, 1.0f);
        check(equalizer_response(bands, kRate, 1000.0) > 11.0, "enabled band shapes response");
        bands[0].enabled = false;
        check(near(equalizer_response(bands, kRate, 1000.0), 0.0, 1e-9),
              "disabled band contributes nothing to the response");
    }
    // Process: disabled band is bit-exact pass-through.
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
    // Disabling one band in a multi-band curve leaves the others active.
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
    // Bank: enabling/disabling the whole clip EQ drops state (matches the
    // existing toggle semantics); per-band disabled bands behave like a flat
    // band at the bank level.
    {
        const int kBankFrames = 24000;
        EqualizerBank bank;
        auto bands = one_band(Clip::EqBand::Type::Notch, 1000.0f, 0.0f, 1.0f);
        auto in = sine(1000.0, kBankFrames);
        auto out = in;
        // Disabled band: the bank cascades nothing -> pass-through.
        bands[0].enabled = false;
        (void)bank.tick(4, bands, true, kRate, 1, out.data(), kBankFrames);
        check(tail_rms_ratio(in, out, kBankFrames) > 0.99,
              "bank with a single disabled band passes through");
        // Re-enabling within the same clip reconfigures to the active notch.
        bands[0].enabled = true;
        auto out2 = in;
        (void)bank.tick(4, bands, true, kRate, 1, out2.data(), kBankFrames);
        check(tail_rms_ratio(in, out2, kBankFrames) < 0.02,
              "re-enabling the band restores the notch law");
    }
}

void test_bank() {
    const int frames = 24000;

    // Clip identity: two clips with different curves get independent filtering.
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

    // Enable toggle drops state: after a disable, re-enable starts a fresh
    // curve — a cold filter, not a continuation of the pre-toggle IIR memory.
    {
        EqualizerBank bank;
        const auto bands = one_band(Clip::EqBand::Type::Bell, 1000.0f, 12.0f, 1.0f);
        const int seg = 5000;
        auto first = sine(1000.0, seg);
        auto fout = first;
        (void)bank.tick(7, bands, true, kRate, 1, fout.data(), seg);
        auto second = sine(1000.0, seg);
        auto sout = second;
        (void)bank.tick(7, bands, false, kRate, 1, sout.data(), seg);
        (void)bank.tick(7, bands, true, kRate, 1, sout.data(), seg);
        // A separate cold-start run is the reference:
        ParametricEqualizer fresh;
        (void)fresh.configure(kRate, 1, bands);
        auto ref_in = second;
        fresh.process(ref_in.data(), seg);
        float worst = 0.0f;
        for (int i = 0; i < seg; ++i)
            worst = std::max(worst, std::fabs(sout[static_cast<std::size_t>(i)] -
                                              ref_in[static_cast<std::size_t>(i)]));
        check(worst < 1e-5f, "disable->enable restarts from a fresh filter");
        // And the pre-disable run WAS affected (the toggle test is meaningful):
        check(!near(static_cast<double>(fout[10]), static_cast<double>(first[10]), 1e-6),
              "pre-disable run was actually filtered");
    }

    // Mid-stream band edit reconfigures: switching to a flat equalizer makes
    // the very next sample a bit-exact pass-through (no stale curve tail).
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

    // Mid-stream GAIN drag while the curve stays active: the ghost of the new
    // curve must NOT cold-start from a zero state. That is the Inspector EQ
    // drag-while-playing gesture — with a cold filter the first sample after
    // the boundary jumps to ~b0*x (a click/static per dragged commit). The
    // state carry ties the new coefficients to the running filter, so the
    // output stays sample-continuous. Find the max per-sample step across the
    // reconfig boundary in the carried run; a cold start makes that step
    // enormous, the carry keeps it near the signal's own slew.
    {
        EqualizerBank bank;
        const int seg = 8000;
        const auto boost_12 = one_band(Clip::EqBand::Type::Bell, 1000.0f, 12.0f, 1.0f);
        const auto boost_14 = one_band(Clip::EqBand::Type::Bell, 1000.0f, 14.0f, 1.0f);
        auto carried = sine(1000.0, seg);
        (void)bank.tick(10, boost_12, true, kRate, 1, carried.data(), seg / 2 - 64);
        // Reconfigure with the dragged curve MID-message: the boundary is the
        // first sample of this new tick (t = seg/2 - 64 in output time).
        (void)bank.tick(10, boost_14, true, kRate, 1, carried.data() + seg / 2 - 64,
                        seg / 2 + 64);
        float max_step = 0.0f;
        for (int i = seg / 2 - 65; i < seg / 2 + 2; ++i)
            max_step = std::max(max_step,
                                std::fabs(static_cast<float>(carried[i]) -
                                          static_cast<float>(carried[i - 1])));
        // A cold start at 12->14 dB settle would step roughly b0(14dB)*0.5-ish
        // (~1.4) on the first post-boundary sample; a smooth carry stays near
        // the sine's own slew (2*pi*1000/48000*0.5 ~ 0.065).
        check(max_step < 0.25,
              "mid-stream gain drag stays sample-continuous (no cold-start click)");
    }

    // drop() on a seek: the next tick re-filters from clean state (no crash,
    // and the law still holds afterwards).
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

    // Rate reach: same bank works across rates (exports may differ from 48 kHz).
    // The tone must be synthesized on the bank's clock so it actually sits on
    // the notch — a 44.1 kHz filter nulls 1 kHz in 44.1 kHz sampled terms.
    {
        EqualizerBank bank;
        const auto bands = one_band(Clip::EqBand::Type::Notch, 1000.0f, 0.0f, 1.0f);
        auto in = sine(1000.0, frames, 0.5, 1, 44100);
        auto out = in;
        (void)bank.tick(5, bands, true, 44100, 1, out.data(), frames);
        check(tail_rms_ratio(in, out, frames) < 0.02,
              "bank runs the notch at 44.1 kHz too");
    }

    // Channel-count flip for the SAME clip with UNCHANGED bands must
    // reconfigure, not stride a buffer with stale cached geometry. The real
    // playback mix does exactly this: a mono source panned (WSOLA up-mix to a
    // stereo front pair while pan != 0) then pan reset (decoder's mono again)
    // keeps identical eq_bands on both sides. process() walks by its cached
    // channel count, so a stale config would read/write past the end of the
    // short buffer (the deterministic heap-garbage bug) — pin the reconfigure.
    {
        EqualizerBank bank;
        const auto bands = one_band(Clip::EqBand::Type::Bell, 1000.0f, 12.0f, 1.0f);
        const int seg = 6000;

        // Stereo tick first (panned mono up-mixed): each channel boosted 3.98x.
        auto st_in = sine(1000.0, seg, 0.5, 2);
        auto st_out = st_in;
        (void)bank.tick(11, bands, true, kRate, 2, st_out.data(), seg);
        check(near(tail_rms_ratio(st_in, st_out, seg, 2), 3.981, 0.08),
              "stereo tick before channel flip boosts");
        check(std::isfinite(static_cast<double>(st_out[seg * 2 - 1])),
              "stereo tick output is finite");

        // Monophonic tick for the SAME clip id, SAME bands: must re-configure
        // to the mono geometry instead of striding the mono buffer as 2ch.
        auto mo_in = sine(1000.0, seg);
        auto mo_out = mo_in;
        (void)bank.tick(11, bands, true, kRate, 1, mo_out.data(), seg);
        check(near(tail_rms_ratio(mo_in, mo_out, seg), 3.981, 0.08),
              "mono tick after stereo config reconfigures (no stale stride)");
        // The state carry: configure() now preserves the DF2T delay across a
        // coefficient/geometry change so a live band edit can't click. Carried
        // state produces a transient at the flip that decays over the filter's
        // time constant — so the mono output must CONVERGE to a fresh cold-start
        // mono filter by the tail (and match the law in steady state), instead
        // of differing forever as a stale-geometry stride would.
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

        // And back to stereo again: reconfigure back to 2ch.
        auto st2_in = sine(1000.0, seg, 0.5, 2);
        auto st2_out = st2_in;
        (void)bank.tick(11, bands, true, kRate, 2, st2_out.data(), seg);
        check(near(tail_rms_ratio(st2_in, st2_out, seg, 2), 3.981, 0.08),
              "stereo-after-mono reconfigures back to 2ch");
    }
}

}  // namespace

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