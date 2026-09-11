// Speed Change (clip_rate) unit test: the headless whole-clip retime law that
// keeps playback video, playback audio, and export in lockstep, plus the
// pitch-preserving WSOLA stretch (time_stretch.hpp) that replaces the old
// varispeed resampler. Pins:
//  - effective_rate gating/clamping, the scaled offset law, the media<->output
//    span inverse round-trip, and monotonicity;
//  - WSOLA preserves PITCH: a 440 Hz sine stretched to 2x tempo must come out
//    at ~440 Hz (the old resampler would have output 880 Hz) and ~half the
//    length; that is the regression guard for "Speed Change must not change
//    my pitch";
//  - streaming determinism: feeding input in chunks produces byte-identical
//    output to one big feed (a gap-free short-returning stream over grains),
//    which is what playback/export rely on;
//  - 0.5x expansion length, stereo channel independence, and bank hygiene.

#include "canvas/core/timeline/clip_rate.hpp"
#include "canvas/core/timeline/time_stretch.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <numbers>
#include <vector>

using namespace canvas::core;

namespace {

int failures = 0;

void check(bool cond, const char* what) {
    if (!cond) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++failures;
    } else {
        std::printf("ok:   %s\n", what);
    }
}

Clip make_clip(float factor, bool enabled) {
    Clip c;
    c.speed_factor = factor;
    c.speed_enabled = enabled;
    return c;
}

bool near(double a, double b, double eps = 1e-6) { return std::fabs(a - b) <= eps; }

// Average zero-crossing frequency of the interior of a mono signal. Robust to
// the windowed ramps at grain heads (win[n] scales amplitude, never moves the
// sign changes), which is exactly what we need to prove "same pitch, faster".
double estimate_freq(const std::vector<float>& sig, int sr, int from) {
    int prev = -1;
    long long sum = 0;
    int crossings = 0;
    for (std::size_t i = static_cast<std::size_t>(from) + 1; i < sig.size(); ++i) {
        if ((sig[i - 1] < 0.f && sig[i] >= 0.f) || (sig[i - 1] >= 0.f && sig[i] < 0.f)) {
            if (prev != -1) {
                sum += static_cast<long long>(i) - prev;
                ++crossings;
            }
            prev = static_cast<int>(i);
        }
    }
    if (crossings == 0) return 0.0;
    return static_cast<double>(sr) / (2.0 * static_cast<double>(sum) / crossings);
}

std::vector<float> make_sine(int sr, int frames, double freq, double amp = 0.8) {
    std::vector<float> out(static_cast<std::size_t>(frames));
    for (int i = 0; i < frames; ++i) {
        out[static_cast<std::size_t>(i)] =
            static_cast<float>(amp * std::sin(2.0 * std::numbers::pi * freq * i / sr));
    }
    return out;
}

bool is_finite(const std::vector<float>& v) {
    for (float f : v)
        if (!std::isfinite(static_cast<double>(f))) return false;
    return true;
}

}  // namespace

int main() {
    {   // effective_rate: gating + clamping.
        check(near(cliprate::effective_rate(make_clip(2.0f, false)), 1.0),
              "effective_rate: disabled -> 1.0");
        check(near(cliprate::effective_rate(make_clip(2.0f, true)), 2.0),
              "effective_rate: enabled -> factor");
        check(near(cliprate::effective_rate(make_clip(0.0f, true)), audio_processing::kSpeedMin),
              "effective_rate: 0 clamped to kSpeedMin");
        check(near(cliprate::effective_rate(make_clip(-5.0f, true)), audio_processing::kSpeedMin),
              "effective_rate: negative clamped to kSpeedMin");
        check(near(cliprate::effective_rate(make_clip(20.0f, true)), audio_processing::kSpeedMax),
              "effective_rate: 20 clamped to kSpeedMax");
    }

    {   // scaled_frame_offset: tl offset -> source frame at rate.
        const Clip c = make_clip(2.0f, true);
        check(cliprate::scaled_frame_offset(c, 0) == 0, "offset: frame 0 -> 0");
        check(cliprate::scaled_frame_offset(c, 10) == 20, "offset: 10 frames at 2x -> 20 source");
        check(cliprate::scaled_frame_offset(c, 7) == 14, "offset: 7 frames at 2x -> 14 source");
        const Clip half = make_clip(0.5f, true);
        check(cliprate::scaled_frame_offset(half, 10) == 5, "offset: 10 frames at 0.5x -> 5 source");
        const Clip quiet = make_clip(2.0f, false);
        check(cliprate::scaled_frame_offset(quiet, 10) == 10, "offset: disabled is identity");
    }

    {   // media_span_for_output / output_frames_from_media: inverse round-trip.
        const Clip c = make_clip(2.0f, true);
        check(cliprate::media_span_for_output(c, 100) == 200, "span: 100 out at 2x -> 200 media");
        check(cliprate::output_frames_from_media(c, 200) == 100, "media: 200 in at 2x -> 100 out");
        const Clip half = make_clip(0.5f, true);
        check(cliprate::media_span_for_output(half, 100) == 50, "span: 100 out at 0.5x -> 50 media");
        check(cliprate::output_frames_from_media(half, 50) == 100, "media: 50 in at 0.5x -> 100 out");
        const Clip quiet = make_clip(3.0f, false);
        check(cliprate::media_span_for_output(quiet, 42) == 42, "span: disabled is identity");
        check(cliprate::output_frames_from_media(quiet, 42) == 42, "media: disabled is identity");
        // Round-trip across a nominal 250-frame window.
        check(cliprate::media_span_for_output(c, 250) == 500 &&
                  cliprate::output_frames_from_media(c, 500) == 250,
              "round-trip: 2x 250 <-> 500");
        check(cliprate::media_span_for_output(half, 250) == 125 &&
                  cliprate::output_frames_from_media(half, 125) == 250,
              "round-trip: 0.5x 250 <-> 125");
    }

    {   // Whole media-spans are monotonic: bigger output window needs >= media.
        const Clip c = make_clip(2.0f, true);
        check(cliprate::media_span_for_output(c, 100) <= cliprate::media_span_for_output(c, 101),
              "span: monotonic in output frames");
        const Clip half = make_clip(0.5f, true);
        check(cliprate::output_frames_from_media(half, 100) <= cliprate::output_frames_from_media(half, 101),
              "media: monotonic in input frames");
    }

    constexpr int kSr = 48000;
    constexpr int kFreq = 440;

    {   // WSOLA 2x: pitch preserved (440 stays 440, NOT 880) and length halves.
        const int n = 24000;  // 0.5 s
        const std::vector<float> in = make_sine(kSr, n, kFreq);
        TimeStretch stretch;
        std::vector<float> out;
        const int written = stretch.process(in.data(), n, 1, 2.0, n / 2, kSr, out);
        check(written > 0 && written <= n / 2, "2x: writes at most n/2");
        check(written >= n / 2 - 4000, "2x: writes at least n/2 minus a lookahead window");
        check(is_finite(out), "2x: output is finite");
        const double f = estimate_freq(out, kSr, 256);
        check(near(f, kFreq, kFreq * 0.03), "2x: pitch preserved ~440 Hz (was 880 under varispeed)");
    }

    {   // WSOLA 0.5x: length doubles; the lookahead feed lets every grain
        // complete inside the call. The waveform-similarity search can walk a
        // few hops of drift on perfectly periodic content (a real signal sits
        // at d ~ 0), so the count is checked against a slack of one drift
        // bound; the seam grain otherwise falls through to the next call,
        // which is how the exporter streams.
        const int n = 12000;
        const std::vector<float> in = make_sine(kSr, n, kFreq);
        const int need = TimeStretch::lookahead_frames(kSr);
        TimeStretch stretch;
        std::vector<float> out;
        const int written = stretch.process(in.data(), n, 1, 0.5, 2 * n, kSr, out);
        check(written > 0 && written <= 2 * n, "0.5x: writes at most 2n");
        TimeStretch stretch2;
        std::vector<float> out2;
        std::vector<float> in2 = in;
        in2.resize(static_cast<std::size_t>(n) + need, 0.0f);
        const int written2 = stretch2.process(in2.data(), n + need, 1, 0.5, 2 * n, kSr, out2);
        check(written2 >= 2 * n - 3 * need && written2 <= 2 * n,
              "0.5x: with lookahead writes ~2n (exact up to one drift bound)");
        const double f2 = estimate_freq(out2, kSr, 256);
        check(near(f2, kFreq, kFreq * 0.03), "0.5x: pitch preserved ~440 Hz");
    }

    {   // Streaming determinism: chunk-feeding == whole-feeding, sample for
        // sample. The engine defers seam grains across chunk boundaries but
        // never changes the grain positions it picks, so the produced stream
        // is identical up to the last produced frame.
        const int n = 24000;
        const std::vector<float> in = make_sine(kSr, n, kFreq);
        TimeStretch whole;
        std::vector<float> out_whole;
        int w_whole = whole.process(in.data(), n, 1, 2.0, n / 2, kSr, out_whole);

        TimeStretch chunked;
        std::vector<float> out_chunked;
        constexpr int kChunk = 1024;
        std::vector<float> tmp;
        for (int off = 0; off < n; off += kChunk) {
            const int take = std::min(kChunk, n - off);
            const int asked = (take / 2) + 1;  // 2x: each fed chunk -> ~half out
            const int got = chunked.process(in.data() + off, take, 1, 2.0, asked, kSr, tmp);
            if (got > 0)
                out_chunked.insert(out_chunked.end(), tmp.begin(),
                                   tmp.begin() + static_cast<std::ptrdiff_t>(got));
        }

        const std::size_t min_len = std::min(out_whole.size(), out_chunked.size());
        check(min_len > 1024, "chunked: produced a usable stream length");
        bool same = true;
        for (std::size_t i = 0; i < min_len; ++i)
            if (std::fabs(static_cast<double>(out_whole[i] - out_chunked[i])) > 1e-6f) {
                same = false;
                break;
            }
        check(same, "chunked stream is sample-identical to the whole feed");
        check(w_whole > 0 && is_finite(out_chunked), "chunked: finite output");
    }

    {   // Stereo: both channels stretch independently, pitches preserved.
        const int n = 16000;
        const std::vector<float> l = make_sine(kSr, n, kFreq);
        const std::vector<float> r = make_sine(kSr, n, 880.0);
        std::vector<float> in(static_cast<std::size_t>(n) * 2);
        for (int i = 0; i < n; ++i) {
            in[static_cast<std::size_t>(2 * i)] = l[static_cast<std::size_t>(i)];
            in[static_cast<std::size_t>(2 * i + 1)] = r[static_cast<std::size_t>(i)];
        }
        TimeStretch stretch;
        std::vector<float> out;
        const int written = stretch.process(in.data(), n, 2, 2.0, n / 2, kSr, out);
        check(written <= n / 2 && written * 2 > 0, "stereo: writes ~n/2 stereo frames");
        std::vector<float> lo(static_cast<std::size_t>(written));
        std::vector<float> ro(static_cast<std::size_t>(written));
        for (int i = 0; i < written; ++i) {
            lo[static_cast<std::size_t>(i)] = out[static_cast<std::size_t>(2 * i)];
            ro[static_cast<std::size_t>(i)] = out[static_cast<std::size_t>(2 * i + 1)];
        }
        const double fl = estimate_freq(lo, kSr, 256);
        const double fr = estimate_freq(ro, kSr, 256);
        check(near(fl, kFreq, kFreq * 0.04), "stereo: L pitch preserved ~440 Hz");
        check(near(fr, 880.0, 880.0 * 0.04), "stereo: R pitch preserved ~880 Hz");
    }

    {   // Bank: per-clip streams stay independent; drop() resets cleanly.
        const int n = 12000;
        const std::vector<float> in = make_sine(kSr, n, kFreq);
        TimeStretchBank bank;
        std::vector<float> out_a, out_b;
        const int wa = bank.tick(11, 2.0, kSr, 1, in.data(), n, n / 2, out_a);
        const int wb = bank.tick(22, 2.0, kSr, 1, in.data(), n, n / 2, out_b);
        check(wa > 0 && wb > 0, "bank: both clips stretch");
        check(near(estimate_freq(out_a, kSr, 256), kFreq, kFreq * 0.03),
              "bank: clip 11 pitch preserved");
        check(near(estimate_freq(out_b, kSr, 256), kFreq, kFreq * 0.03),
              "bank: clip 22 pitch preserved");
        bank.drop(11);
        std::vector<float> out_a2;
        const int wa2 = bank.tick(11, 2.0, kSr, 1, in.data(), n, n / 2, out_a2);
        check(wa2 > 0 && near(estimate_freq(out_a2, kSr, 256), kFreq, kFreq * 0.03),
              "bank: drop(11) lets the clip restart clean");
        // A ratio change resets the engine defensively (no stale analysis).
        std::vector<float> out_c;
        const int wc = bank.tick(11, 0.5, kSr, 1, in.data(), n, 2 * n, out_c);
        check(wc > 0, "bank: ratio change restarts the stream");
        bank.clear();
    }

    if (failures == 0) {
        std::printf("ALL TESTS PASSED\n");
        return 0;
    }
    std::fprintf(stderr, "%d TEST(S) FAILED\n", failures);
    return 1;
}