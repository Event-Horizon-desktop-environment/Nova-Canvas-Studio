// Equalizer test: pins the 6-band parametric EQ law (RBJ Audio EQ Cookbook
// biquads via the shared headless seam in equalizer.hpp). Covers the cascade
// magnitude response (equalizer_response), the streaming process() law per
// filter type, chunked-vs-whole streaming equivalence, per-clip bank behavior
// (clip independence, enable-toggle state KEEP, mid-stream band edits), and
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
    // Per-band response: a single band's biquad matches its contribution to the
    // composite curve. For f0-spaced bells the |H|² responses multiply, so the
    // composite dB is exactly the sum of the per-band dB wherever the band
    // shapes do not interact. A disabled band contributes exactly 0 dB — even
    // if its other settings would shape — and re-enabling restores it.
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

    // Enable toggle KEEPS filter state: after a disable (bit-exact pass-
    // through), re-enable resumes the same per-clip entry — the carried DF2T
    // state, not a cold-start. A zero-state restart would jump the first
    // sample to ~b0*x (a click/static per toggle); FreeEQ8 keeps a disabled
    // band's state until reset()/prepareToPlay. drop() is the explicit clear.
    {
        EqualizerBank bank;
        const auto bands = one_band(Clip::EqBand::Type::Bell, 1000.0f, 12.0f, 1.0f);
        const int seg = 5000;
        // ONE long phase-continuous tone, sliced so every run is a continuation
        // of the previous one: the edge checks below measure the toggle's own
        // step, so the underlying signal must not have an artificial phase jump
        // at the slice boundaries.
        const auto whole = sine(1000.0, 3 * seg);

        auto first = std::vector<float>(whole.begin(), whole.begin() + seg);
        auto fout = first;
        (void)bank.tick(7, bands, true, kRate, 1, fout.data(), seg);
        // Disable edge must be a CROSSFADE, not a hard one-sample snap: the
        // pre-glide toggle measured a ~0.58-1.3 step at the boundary (the
        // audible boom); gliding shaped-into-dry keeps it near the signal's own
        // slew (2*pi*1000/48000 * ~2.0 ~ 0.26 for the +12 dB bell at full wet).
        auto gapped = std::vector<float>(whole.begin() + seg, whole.begin() + 2 * seg);
        auto gout = gapped;
        (void)bank.tick(7, bands, false, kRate, 1, gout.data(), seg);
        const float disable_edge = std::fabs(static_cast<float>(gout[0]) -
                                             static_cast<float>(fout[seg - 1]));
        check(disable_edge < 0.4f, "disable edge crossfades (no one-sample boom)");
        // Outside the kGlideFrames crossfade the disabled region is a byte-exact
        // pass-through (the settle target is dry).
        bool dry_exact = true;
        for (int i = ParametricEqualizer::kGlideFrames; i < seg; ++i) {
            if (gout[static_cast<std::size_t>(i)] != gapped[static_cast<std::size_t>(i)]) {
                dry_exact = false;
                break;
            }
        }
        check(dry_exact,
              "disabled region is byte-exact pass-through after the crossfade window");
        // Re-enable on a phase-continuous continuation: the dry->wet glide and
        // the carried state must resume the filter close to a never-interrupted
        // run, and measurably differ from a cold restart. The re-enable edge
        // itself must also glide (no boom on the way back in).
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
        // The glide now keeps the first kGlideFrames of the re-enable mostly
        // dry while `ref` is fully wet from frame 0, so the difference is even
        // larger than the pure state-carry difference it used to measure —
        // exactly the extra evidence that the resume crossfades instead of
        // snapping from raw straight into shaped output.
        check(worst_vs_fresh > 1e-4f,
              "re-enable resumes carried state, not a cold start (glide present)");
        // And it still converges to the +12 dB law by the tail.
        check(near(tail_rms_ratio(cont_in, carried, seg), 3.981, 0.08),
              "re-enable restores the +12 dB bell law (steady state)");
        // The pre-disable run WAS filtered past its own dry->wet glide (the
        // toggle test is meaningful).
        check(!near(static_cast<double>(fout[200]), static_cast<double>(first[200]), 1e-6),
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
    // drag-while-playing gesture — with a cold filter the first samples after
    // the boundary jump away from the carried output (a click/static per
    // dragged commit). The state carry ties the new coefficients to the
    // running filter, and the coefficient glide starts at t=0 on the OLD
    // cascade, so the output stays sample-continuous.
    //
    // The old check compared the raw max per-sample step around the boundary
    // to a fixed 0.25 cap — and failed against a provably continuous run: the
    // 12->14 dB boost grows the 0.5-amplitude 1 kHz sine to ~2.0-2.5, whose
    // intrinsic per-sample slew (2.51 * 2pi*1000/48000 ~ 0.33) exceeds 0.25
    // all on its own. So the drag is pinned against reference runs instead:
    //   1. the boundary sample is bit-identical to a NEVER-reconfigured +12
    //      run (glide t=0 is exactly the old cascade and the DF2T state is
    //      carried, so the first post-drag sample is the pure continuation);
    //   2. the next 16 samples stay within glide-drift of that +12
    //      continuation — a zero-state cold start diverges by O(1) here
    //      (measured 1.06 for the cold start vs 0.014 for the carry),
    //      pinning the state carry;
    //   3. the run still converges to the +14 dB law (10^(14/20) = 5.012x)
    //      in steady state, pinning the glide target.
    {
        EqualizerBank bank;
        const int seg = 8000;
        const auto boost_12 = one_band(Clip::EqBand::Type::Bell, 1000.0f, 12.0f, 1.0f);
        const auto boost_14 = one_band(Clip::EqBand::Type::Bell, 1000.0f, 14.0f, 1.0f);
        const auto in = sine(1000.0, seg);
        const int boundary = seg / 2 - 64;  // tick 2 (the drag) starts here
        std::vector<float> carried = in;
        (void)bank.tick(10, boost_12, true, kRate, 1, carried.data(), boundary);
        (void)bank.tick(10, boost_14, true, kRate, 1, carried.data() + boundary,
                        seg - boundary);

        auto ref12 = in;  // the same sine with +12 dB FOREVER: no drag at all
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

    // Mid-stream band edit through the IDENTITY threshold: dragging a MIDDLE
    // active band's gain to ~0 dB (or toggling a band off) while others stay
    // active. This used to DROP the band from the cascade (build_cascade
    // skipped non-filtering slots), changing the stage count: configure()
    // snapped the coefficients AND the positional state-carry shifted every
    // carried DF2T state after the dropped slot into a misaligned stage — a
    // hard pop at the exact reconfigure boundary (~0.31 single-frame step on a
    // 440 Hz sine whose intrinsic slew is ~0.05). Identity-filled slots keep
    // the cascade at a constant six stages, so the edit glides the slot in
    // place and the step stays near the sine's own slew.
    {
        EqualizerBank bank;
        std::array<Clip::EqBand, 6> bands{};
        for (auto& b : bands) b.type = Clip::EqBand::Type::Bell;
        bands[0].frequency = 800.0f;  bands[0].gain = 12.0f;  bands[0].q = 1.0f;
        bands[2].frequency = 2000.0f; bands[2].gain = 12.0f;  bands[2].q = 1.0f;
        bands[4].frequency = 4000.0f; bands[4].gain = 12.0f;  bands[4].q = 1.0f;

        const int seg = 8000;
        auto in = sine(440.0, seg, 0.9);
        std::vector<float> out = in;
        const int boundary = seg / 2;
        (void)bank.tick(31, bands, true, kRate, 1, out.data(), boundary);

        // The MIDDLE active band crosses into identity; the outer two stay.
        std::array<Clip::EqBand, 6> edit = bands;
        edit[2].gain = 0.0f;
        (void)bank.tick(31, edit, true, kRate, 1, out.data() + boundary, seg - boundary);

        // The single-frame step across the boundary window must stay under a
        // =6x-tolerance over the sine's intrinsic slew (2*pi*440/48000 * 0.9 ~
        // 0.052) — a structural snap lands well above that (measured 0.31).
        float max_step = 0.0f;
        const int win = 64;
        for (int i = boundary - win; i < boundary + win && i < seg; ++i) {
            const float d = std::fabs(out[static_cast<std::size_t>(i)] -
                                      out[static_cast<std::size_t>(i - 1)]);
            max_step = std::max(max_step, d);
        }
        check(max_step < 6.0f * static_cast<float>(2.0 * 3.141592653589793 * 440.0 / kRate * 0.9),
              "identity-threshold band edit glides in place (no structural pop)");

        // Steady state: past the glide the surviving curves match a fresh EQ
        // configured with the edited bands (the 0 dB slot must read as 0 dB).
        ParametricEqualizer fresh;
        (void)fresh.configure(kRate, 1, edit);
        auto ref = in;
        fresh.process(ref.data(), seg);
        float worst = 0.0f;
        for (int i = seg - 1024; i < seg; ++i)
            worst = std::max(worst, std::fabs(out[static_cast<std::size_t>(i)] -
                                              ref[static_cast<std::size_t>(i)]));
        check(worst < 1e-3f,
              "identity-threshold band edit converges to the new law (0 dB slot is identity)");
    }

    // Seek+resume on an ALREADY-ENABLED clip must re-glide, not expose the
    // freshly cold-reset filter at full wet. This is the "pop on (random)
    // startup" regression: drop() cold-resets the filter's IIR state (as it
    // must, for a genuinely discontinuous seek), so the very next enabled
    // tick() has to run the SAME protected dry->wet glide a first-time enable
    // gets — snapping wet_ straight back to 1.0 after the reset would hand the
    // cold filter's own transient to the output unblended. A resonant/cutting
    // band exposes this worst: a HighPass corner well above the signal's
    // content overshoots far past its own steady-state level for the first
    // few ms out of a cold state.
    {
        EqualizerBank bank;
        const auto bands = one_band(Clip::EqBand::Type::HighPass, 800.0f, 0.0f, 3.0f);
        const int seg = 4000;
        // A real seek lands on an arbitrary phase of whatever is playing, not
        // conveniently at a zero-crossing — use a mid-cycle phase offset (a
        // zero-crossing start hides the bug: y[0] = b0*x[0] = b0*0 either way).
        auto make_sig = [&] {
            std::vector<float> s(seg);
            for (int f = 0; f < seg; ++f)
                s[f] = static_cast<float>(0.7 * std::sin(2.0 * 3.141592653589793 * 120.0 * f /
                                                          kRate + 1.7));
            return s;
        };

        auto first = make_sig();
        (void)bank.tick(20, bands, true, kRate, 1, first.data(), seg);  // fresh enable: glide-protected

        bank.drop();  // simulate a seek back to the same clip

        auto raw = make_sig();
        auto resumed = raw;
        (void)bank.tick(20, bands, true, kRate, 1, resumed.data(), seg);  // resume: must re-glide

        // Frame 0 of the resume must be exactly raw (wet_ starts back at 0,
        // same as any fresh enable) — a cold-reset filter must never be
        // exposed at full strength on the very first post-seek sample.
        check(near(static_cast<double>(resumed[0]), static_cast<double>(raw[0]), 1e-5),
              "seek+resume on an enabled clip starts fully dry (re-glides, no exposed cold start)");
        // Nowhere in the glide window should the resumed output meaningfully
        // overshoot the raw input's own amplitude. The glide blends a fraction
        // of the settling filter in from the start, so a small (<~20% of
        // amplitude) blip as the cold filter's transient decays is the
        // expected, harmless residual of a *properly glided* resume; the bug
        // instead exposed the cold HighPass at full wet immediately, spiking
        // tens of times past steady state (measured 42x on this exact case).
        float worst_over = 0.0f;
        for (int i = 0; i < ParametricEqualizer::kGlideFrames; ++i)
            worst_over = std::max(
                worst_over, std::fabs(resumed[static_cast<std::size_t>(i)]) - std::fabs(raw[static_cast<std::size_t>(i)]));
        check(worst_over < 0.15f,
              "seek+resume glide never overshoots the raw signal by more than a small, "
              "properly-glided residual (no exposed cold-start pop)");
        // The curve still settles to the same steady-state law as an
        // uninterrupted run.
        std::vector<float> tail_raw(raw.begin() + 1024, raw.end());
        std::vector<float> tail_out(resumed.begin() + 1024, resumed.end());
        check(tail_rms_ratio(tail_raw, tail_out, static_cast<int>(tail_out.size())) < 0.05,
              "seek+resume still converges to the high-pass law after the glide");
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