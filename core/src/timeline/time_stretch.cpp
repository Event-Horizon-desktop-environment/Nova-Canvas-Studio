// Per-clip retime engine: WSOLA ("Waveform Similarity Overlap-Add") streaming
// time-stretch with a windowed-sinc resampling front-end for pitch shift and
// the shared audio_mix pan law baked into its output. Snapshot of the
// well-known constant-rate algorithm for the stretch stage: the output
// timeline is a lattice of synthesis hops S apart; each grain is one W-sample
// Hann-windowed copy of the source waveform whose analysis start is chosen
// (within a +/-D search radius of the nominal ratio*S advance) to best match
// the previous grain's tail, which keeps the stitched waveform continuous and
// therefore pitch-identical to the source. Only the hop distance differs, so
// the tempo (grains per output second) changes while the pitch inside each
// grain stays.
//
// Overlap-add bookkeeping: with S = W/2 the Hann windows of successive grains
// sum to unity in their S-sample overlap, so each grain emits exactly its S
// head samples (windowed source + the previous grain's windowed tail), then
// its [S..W) tail becomes the overlap carry for the next grain. The engine is
// stateful across calls: an input history ring keeps source samples around for
// windows that reach backward, the analysis/synthesis cursors advance
// monotonically, and a short call simply defers the seam grain until the next
// call's contiguous input arrives.
//
// Pitch shift: a `pitch_factor` p != 1 runs the media through a windowed-sinc
// resampler first (output length /p, pitch xp); the WSOLA stage then consumes
// `speed_ratio / p` resampled frames per output frame, so the two geometric
// rates cancel and the final output length obeys only the speed law while the
// pitch lands on p. When |speed_ratio - p| ~ 0 the WSOLA stage is bypassed
// (its unity-ratio OLA would re-corrupt a clean resample), and when p == 1
// the resampler is skipped outright so the speed-only path is byte-identical
// to the pre-pitch engine.

#include "canvas/core/timeline/time_stretch.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <numbers>

#include "canvas/core/timeline/audio_mix.hpp"

namespace canvas::core {

namespace {

// ~40 ms synthesis window at any rate, forced even so S = W/2 is integral.
int window_samples(int sample_rate) {
    const int w = static_cast<int>(
        2 * std::max(1LL, std::llround(0.04 * static_cast<double>(sample_rate) / 2.0)));
    return w < 256 ? 256 : w;
}

}  // namespace

int TimeStretch::lookahead_frames(int sample_rate) noexcept {
    if (sample_rate <= 0) return 0;
    const int w = window_samples(sample_rate);
    return w + std::max(1, w / 12);
}

int TimeStretch::src_margin_frames() noexcept {
    return kSrcTapsHalf + 1;
}

int TimeStretch::retime_lookahead(int sample_rate, double speed_ratio,
                                  double pitch_factor) noexcept {
    if (sample_rate <= 0 || speed_ratio <= 0.0 || !(pitch_factor > 1e-9)) return 0;
    if (speed_ratio == 1.0 && pitch_factor == 1.0) return 0;
    const int need = lookahead_frames(sample_rate);
    const bool wsola = std::fabs(speed_ratio - pitch_factor) > 1e-9;
    int extra = wsola ? static_cast<int>(std::llround(static_cast<double>(need) *
                                                      pitch_factor))
                      : 0;
    if (pitch_factor != 1.0) extra += src_margin_frames();
    return extra;
}

double TimeStretch::sinc(double x) noexcept {
    if (std::fabs(x) < 1e-9) return 1.0;
    const double px = std::numbers::pi * x;
    return std::sin(px) / px;
}

void TimeStretch::setup(int sample_rate, int channels) {
    if (sample_rate == sample_rate_ && channels == channels_ && W_ > 0) return;
    sample_rate_ = sample_rate;
    channels_ = channels;
    W_ = window_samples(sample_rate);
    S_ = W_ / 2;
    D_ = std::max(1, W_ / 12);
    window_.resize(static_cast<std::size_t>(W_));
    for (int n = 0; n < W_; ++n) {
        window_[static_cast<std::size_t>(n)] = static_cast<float>(
            0.5 - 0.5 * std::cos(2.0 * std::numbers::pi *
                                 n / (W_ - 1)));
    }
    cur_win_.assign(static_cast<std::size_t>(W_) * channels, 0.0f);
    prev_tail_.assign(static_cast<std::size_t>(S_) * channels, 0.0f);
    prev_sig_.assign(static_cast<std::size_t>(S_) * channels, 0.0f);
}

// Drop resampled history at least a full hop before the newest analysis
// position the next grain can reach (the search may step back up to +/-D
// from `a_`). Operates on the SRC-OUTPUT ring (in_hist_).
void TimeStretch::compact_history() {
    const int64_t keep = std::max<int64_t>(0, static_cast<int64_t>(std::floor(a_)) - D_);
    const int64_t drop = keep - in_base_;
    if (drop <= S_ || drop <= 0) return;
    const auto keep_bytes_n = static_cast<std::size_t>(drop) * channels_;
    if (keep_bytes_n >= in_hist_.size()) {
        in_hist_.clear();
    } else {
        in_hist_.erase(in_hist_.begin(),
                       in_hist_.begin() + static_cast<std::ptrdiff_t>(keep_bytes_n));
    }
    in_base_ = keep;
}

// Drop RAW media history the resampler can never need again: its taps reach
// up to kSrcTapsHalf ahead of the cursor and back the same distance, so once
// the cursor has moved past a sample it is dead.
void TimeStretch::compact_media() {
    const int64_t keep =
        std::max<int64_t>(0, static_cast<int64_t>(std::floor(src_pos_)) - kSrcTapsHalf);
    const int64_t drop = keep - media_base_;
    if (drop <= 0) return;
    const std::size_t n = static_cast<std::size_t>(drop) * channels_;
    if (n >= media_hist_.size()) {
        media_hist_.clear();
    } else {
        media_hist_.erase(media_hist_.begin(),
                          media_hist_.begin() + static_cast<std::ptrdiff_t>(n));
    }
    media_base_ = keep;
}

// Windowed-sinc (Lanczos-style, 2*kSrcTapsHalf taps, DC-normalised per
// sample) resample of the raw media ring into the SRC-output ring. The step
// is `pitch`: output sample m reads media at position m*pitch, so the output
// has in/pitch samples (duration /pitch) reconstructed with the signal's
// pitch multiplied by `pitch`. The anti-alias cutoff shrinks to 1/pitch when
// downsampling (pitch > 1); taps that fall off the retained ring (only at
// stream start) are skipped and the remaining taps renormalised.
void TimeStretch::emit_src(int64_t media_end) {
    const double step = pitch_;
    const double alpha = std::min(1.0, 1.0 / pitch_);
    const double inv_h = 1.0 / static_cast<double>(kSrcTapsHalf);
    const int taps = 2 * kSrcTapsHalf;
    float coeff[2 * kSrcTapsHalf];
    for (;;) {
        const int64_t posi = static_cast<int64_t>(std::floor(src_pos_));
        if (posi + kSrcTapsHalf >= media_end) break;
        const int64_t j0 = posi - kSrcTapsHalf + 1;
        double csum = 0.0;
        for (int t = 0; t < taps; ++t) {
            const int64_t j = j0 + t;
            if (j < 0 || j >= media_end) {
                coeff[t] = 0.0f;
                continue;
            }
            const double d = src_pos_ - static_cast<double>(j);
            const double c = sinc(alpha * d) * sinc(d * inv_h);
            coeff[t] = static_cast<float>(c);
            csum += c;
        }
        if (csum > 1e-12) {
            const double inv = 1.0 / csum;
            for (int ch = 0; ch < channels_; ++ch) {
                double acc = 0.0;
                for (int t = 0; t < taps; ++t) {
                    const int64_t j = j0 + t;
                    if (j < 0 || j >= media_end || coeff[t] == 0.0f) continue;
                    const std::size_t off =
                        static_cast<std::size_t>(j - media_base_) * channels_ + ch;
                    acc += static_cast<double>(media_hist_[off]) * coeff[t];
                }
                in_hist_.push_back(static_cast<float>(acc * inv));
            }
        } else {
            for (int ch = 0; ch < channels_; ++ch) in_hist_.push_back(0.0f);
        }
        src_pos_ += step;
    }
}

// WSOLA-inactive mode: the SRC output IS the result (speed == pitch, or a
// pan-only unity stream). Copy up to `out_frames` frames off the front of the
// SRC/output ring into `stage_`, consume them, and advance the ring base.
int TimeStretch::run_passthrough(int out_frames) {
    const int64_t avail = static_cast<int64_t>(in_hist_.size() / channels_);
    const int take = static_cast<int>(std::min<int64_t>(avail, out_frames));
    if (take > 0) {
        const std::size_t n = static_cast<std::size_t>(take) * channels_;
        stage_.resize(n);
        std::copy_n(in_hist_.data(), static_cast<std::ptrdiff_t>(n), stage_.begin());
        in_hist_.erase(in_hist_.begin(),
                       in_hist_.begin() + static_cast<std::ptrdiff_t>(n));
        in_base_ += take;
    }
    return take;
}

int TimeStretch::search_best_offset(int64_t candidate) const {
    int best_d = 0;
    double best_score = -1.0;
    const int64_t end = in_base_ + static_cast<int64_t>(in_hist_.size() / channels_);
    for (int d = -D_; d <= D_; ++d) {
        const int64_t c = candidate + d;
        if (c < in_base_ || c < 0 || c + S_ > end) continue;
        double corr = 0.0;
        double eng = 0.0;
        const float* x = in_hist_.data() + static_cast<std::size_t>(c - in_base_) *
                                                channels_;
        const float* t = prev_sig_.data();
        for (int n = 0; n < S_; ++n) {
            for (int ch = 0; ch < channels_; ++ch) {
                const double a = x[static_cast<std::size_t>(n) * channels_ + ch];
                const double b = t[static_cast<std::size_t>(n) * channels_ + ch];
                corr += a * b;
                eng += a * a;
            }
        }
        const double score = eng > 1e-12 ? corr / std::sqrt(eng) : corr;
        if (score > best_score) {
            best_score = score;
            best_d = d;
        }
    }
    return best_d;
}

// WSOLA synthesis lattice, unchanged in geometry from the speed-only engine,
// reading the SRC-output ring and writing the pre-pan lane buffer `stage_`:
// each grain emits exactly S head samples (windowed source + previous grain's
// windowed tail), then its [S..W) tail becomes the next grain's overlap carry,
// advancing with the nominal-analysis cursor a_.
int TimeStretch::run_synth(int out_frames) {
    stage_.resize(static_cast<std::size_t>(out_frames) * channels_);
    std::fill(stage_.begin(), stage_.end(), 0.0f);
    int written = 0;
    const int64_t avail_end = in_base_ + static_cast<int64_t>(in_hist_.size() / channels_);

    while (written < out_frames) {
        if (!first_) {
            // Continue the current grain's head if we stopped mid-way; each
            // grain emits exactly S samples (windowed source + previous
            // grain's windowed tail), so a partial last call resumes cleanly.
            if (out_base_ < cur_s_ + S_) {
                const int n0 = static_cast<int>(out_base_ - cur_s_);
                const int take = std::min(S_ - n0, out_frames - written);
                for (int n = 0; n < take; ++n) {
                    const std::size_t k = static_cast<std::size_t>(n0 + n);
                    const float* cw = cur_win_.data() + k * channels_;
                    const float* pt = prev_tail_.data() + k * channels_;
                    float* o = stage_.data() + static_cast<std::size_t>(written + n) * channels_;
                    for (int ch = 0; ch < channels_; ++ch) o[ch] = cw[ch] + pt[ch];
                }
                written += take;
                out_base_ += take;
                continue;
            }
            // Else out_base_ == cur_s_ + S_: the previous grain is spent; fall
            // through and place the next one on the synthesis lattice.
        }

        // Place the next grain on the lattice: nominal analysis advance
        // ratio*S from the previous grain, refined by waveform similarity up
        // to +/-D so the new head continues the old tail phase-coherently.
        const int64_t candidate = static_cast<int64_t>(std::llround(a_));
        int best_d = first_ ? 0 : search_best_offset(candidate);
        int64_t a_k = candidate + best_d;
        if (a_k < 0) a_k = 0;
        if (a_k + W_ > avail_end) break;  // need more input; defer seam grain
        const std::size_t grain_off =
            static_cast<std::size_t>(a_k - in_base_) * channels_;
        const float* src = in_hist_.data() + grain_off;

        // This grain's OLA carry for the next one is the current window's
        // tail; its waveform-similarity reference is the current window's raw
        // tail. Grab both before overwriting cur_win_.
        if (!first_) {
            std::copy(cur_win_.begin() + static_cast<std::ptrdiff_t>(S_) * channels_,
                      cur_win_.end(), prev_tail_.begin());
        }
        for (int n = 0; n < W_; ++n) {
            const float w = window_[static_cast<std::size_t>(n)];
            float* cw = cur_win_.data() + static_cast<std::size_t>(n) * channels_;
            for (int ch = 0; ch < channels_; ++ch) cw[ch] = w * src[static_cast<std::size_t>(n) * channels_ + ch];
        }
        if (!first_) {
            std::copy(src + static_cast<std::ptrdiff_t>(S_) * channels_,
                      src + static_cast<std::ptrdiff_t>(W_) * channels_,
                      prev_sig_.begin());
        }

        cur_s_ = s_;
        s_ += S_;
        a_ = static_cast<double>(a_k) + wsola_ratio_ * static_cast<double>(S_);
        first_ = false;
        compact_history();
    }

    return written;
}

// Push the engine-lane `stage_` into `out` with the pan balance and the mono
// upmix. The pan law is audio_mix::pan_gains — the SAME law the boundary
// mix still applies to clips the engine never touched — so a panned source is
// applied exactly once regardless of which path rendered it.
void TimeStretch::finalize(std::vector<float>& out, int written, float pan) {
    if (written <= 0) return;
    const std::size_t n = static_cast<std::size_t>(written);
    if (out_channels_ == channels_ && pan == 0.0f) {
        std::copy_n(stage_.data(), static_cast<std::ptrdiff_t>(n) * channels_,
                    out.data());
        return;
    }
    float gl = 1.0f, gr = 1.0f;
    audio_mix::pan_gains(pan, gl, gr);
    const std::size_t och = static_cast<std::size_t>(out_channels_);
    for (std::size_t f = 0; f < n; ++f) {
        const std::size_t so = f * static_cast<std::size_t>(channels_);
        float* o = out.data() + f * och;
        if (channels_ == 1) {
            o[0] = stage_[so] * gl;
            o[1] = stage_[so] * gr;
        } else {
            o[0] = stage_[so] * gl;
            o[1] = stage_[so + 1] * gr;
            for (int c = 2; c < out_channels_; ++c)
                o[c] = stage_[so + static_cast<std::size_t>(c)];
        }
    }
}

int TimeStretch::process(const float* in, int in_frames, int channels, double speed_ratio,
                         double pitch_factor, float pan, int out_frames, int sample_rate,
                         std::vector<float>& out) {
    if (in == nullptr || in_frames <= 0 || channels <= 0 || out_frames <= 0 ||
        speed_ratio <= 0.0 || !(pitch_factor > 1e-9) || sample_rate <= 0) {
        out.clear();
        return 0;
    }
    pitch_ = pitch_factor;
    wsola_ratio_ = speed_ratio / pitch_factor;
    setup(sample_rate, channels);
    if (W_ <= 0 || S_ <= 0 || D_ <= 0) {
        out.clear();
        return 0;
    }

    const std::size_t add = static_cast<std::size_t>(in_frames) * channels;
    const bool srcing = std::fabs(pitch_factor - 1.0) > 1e-9;
    if (srcing) {
        // Resampled path: hold the raw media ring, stream it through the SRC
        // into the WSOLA/output ring.
        media_hist_.insert(media_hist_.end(), in, in + static_cast<std::ptrdiff_t>(add));
        emit_src(media_base_ + static_cast<int64_t>(media_hist_.size() / channels));
        compact_media();
    } else {
        // Unity pitch: the input history IS the output ring (the speed-only
        // and pan-only paths, byte-identical to the pre-pitch engine).
        in_hist_.insert(in_hist_.end(), in, in + static_cast<std::ptrdiff_t>(add));
    }

    out_channels_ = (channels == 1 && pan != 0.0f) ? 2 : channels;
    out.assign(static_cast<std::size_t>(out_frames) * out_channels_, 0.0f);

    const bool wsola = std::fabs(speed_ratio - pitch_factor) > 1e-9;
    const int written = wsola ? run_synth(out_frames) : run_passthrough(out_frames);
    finalize(out, written, pan);
    return written;
}

void TimeStretch::reset() {
    std::vector<float>().swap(in_hist_);
    std::vector<float>().swap(media_hist_);
    std::vector<float>().swap(stage_);
    std::vector<float>().swap(cur_win_);
    std::vector<float>().swap(prev_tail_);
    std::vector<float>().swap(prev_sig_);
    std::vector<float>().swap(window_);
    in_base_ = 0;
    media_base_ = 0;
    src_pos_ = 0.0;
    pitch_ = 1.0;
    wsola_ratio_ = 1.0;
    out_channels_ = 0;
    cur_s_ = 0;
    s_ = 0;
    out_base_ = 0;
    a_ = 0.0;
    first_ = true;
    W_ = S_ = D_ = 0;
    sample_rate_ = 0;
    channels_ = 0;
}

int TimeStretchBank::tick(std::uint64_t clip_id, double speed_ratio, double pitch_factor,
                          float pan, int sample_rate, int channels, const float* in,
                          int in_frames, int out_frames, std::vector<float>& out,
                          int* out_channels) {
    Entry& e = entries_[clip_id];
    if (e.ratio != 0.0 && (e.ratio != speed_ratio || e.pitch != pitch_factor)) {
        // A clip's speed or pitch changing mid-stream tears the timeline law
        // apart; drop the old stream so the new pair starts clean.
        e.engine.reset();
    }
    e.ratio = speed_ratio;
    e.pitch = pitch_factor;
    const int written = e.engine.process(in, in_frames, channels, speed_ratio,
                                         pitch_factor, pan, out_frames, sample_rate, out);
    if (out_channels) *out_channels = e.engine.out_channels();
    return written;
}

void TimeStretchBank::drop() {
    for (auto& [id, e] : entries_) {
        (void)id;
        e.engine.reset();
    }
}

void TimeStretchBank::drop(std::uint64_t clip_id) {
    const auto it = entries_.find(clip_id);
    if (it != entries_.end()) it->second.engine.reset();
}

void TimeStretchBank::clear() { entries_.clear(); }

}  // namespace canvas::core