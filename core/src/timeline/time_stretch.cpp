// WSOLA ("Waveform Similarity Overlap-Add") streaming time-stretch. Snapshot
// of the well-known constant-rate algorithm: the output timeline is a lattice
// of synthesis hops S apart; each grain is one W-sample Hann-windowed copy of
// the source waveform whose analysis start is chosen (within a +/-D search
// radius of the nominal ratio*S advance) to best match the previous grain's
// tail, which keeps the stitched waveform continuous and therefore
// pitch-identical to the source. Only the hop distance differs, so the tempo
// (grains per output second) changes while the pitch inside each grain stays.
//
// Overlap-add bookkeeping: with S = W/2 the Hann windows of successive grains
// sum to unity in their S-sample overlap, so each grain emits exactly its S
// head samples (windowed source + the previous grain's windowed tail), then
// its [S..W) tail becomes the overlap carry for the next grain. The engine is
// stateful across calls: an input history ring keeps source samples around for
// windows that reach backward, the analysis/synthesis cursors advance
// monotonically, and a short call simply defers the seam grain until the next
// call's contiguous input arrives.

#include "canvas/core/timeline/time_stretch.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <numbers>

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

// Drop history at least a full hop before the newest analysis position the
// next grain can reach (the search may step back up to +/-D from `a_`).
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

int TimeStretch::process(const float* in, int in_frames, int channels, double ratio,
                         int out_frames, int sample_rate, std::vector<float>& out) {
    if (in == nullptr || in_frames <= 0 || channels <= 0 || out_frames <= 0 || ratio <= 0.0 ||
        sample_rate <= 0) {
        out.clear();
        return 0;
    }
    setup(sample_rate, channels);
    if (W_ <= 0 || S_ <= 0 || D_ <= 0) {
        out.clear();
        return 0;
    }

    // Append the caller's (contiguous) input to the history ring.
    const std::size_t add = static_cast<std::size_t>(in_frames) * channels;
    in_hist_.insert(in_hist_.end(), in, in + static_cast<std::ptrdiff_t>(add));

    out.resize(static_cast<std::size_t>(out_frames) * channels);
    std::fill(out.begin(), out.end(), 0.0f);
    int written = 0;
    const int64_t avail_end = in_base_ + static_cast<int64_t>(in_hist_.size() / channels);

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
                    const float* cw = cur_win_.data() + k * channels;
                    const float* pt = prev_tail_.data() + k * channels;
                    float* o = out.data() + static_cast<std::size_t>(written + n) * channels;
                    for (int ch = 0; ch < channels; ++ch) o[ch] = cw[ch] + pt[ch];
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
            static_cast<std::size_t>(a_k - in_base_) * channels;
        const float* src = in_hist_.data() + grain_off;

        // This grain's OLA carry for the next one is the current window's
        // tail; its waveform-similarity reference is the current window's raw
        // tail. Grab both before overwriting cur_win_.
        if (!first_) {
            std::copy(cur_win_.begin() + static_cast<std::ptrdiff_t>(S_) * channels,
                      cur_win_.end(), prev_tail_.begin());
        }
        for (int n = 0; n < W_; ++n) {
            const float w = window_[static_cast<std::size_t>(n)];
            float* cw = cur_win_.data() + static_cast<std::size_t>(n) * channels;
            for (int ch = 0; ch < channels; ++ch) cw[ch] = w * src[static_cast<std::size_t>(n) * channels + ch];
        }
        if (!first_) {
            std::copy(src + static_cast<std::ptrdiff_t>(S_) * channels,
                      src + static_cast<std::ptrdiff_t>(W_) * channels,
                      prev_sig_.begin());
        }

        cur_s_ = s_;
        s_ += S_;
        a_ = static_cast<double>(a_k) + ratio * static_cast<double>(S_);
        first_ = false;
        compact_history();
    }

    return written;
}

void TimeStretch::reset() {
    std::vector<float>().swap(in_hist_);
    std::vector<float>().swap(cur_win_);
    std::vector<float>().swap(prev_tail_);
    std::vector<float>().swap(prev_sig_);
    std::vector<float>().swap(window_);
    in_base_ = 0;
    cur_s_ = 0;
    s_ = 0;
    out_base_ = 0;
    a_ = 0.0;
    first_ = true;
    W_ = S_ = D_ = 0;
    sample_rate_ = 0;
    channels_ = 0;
}

int TimeStretchBank::tick(std::uint64_t clip_id, double ratio, int sample_rate,
                          int channels, const float* in, int in_frames, int out_frames,
                          std::vector<float>& out) {
    Entry& e = entries_[clip_id];
    if (e.ratio != 0.0 && e.ratio != ratio) {
        // A clip's speed changing mid-stream tears the timeline law apart;
        // drop the old stream so the new ratio starts clean.
        e.engine.reset();
    }
    e.ratio = ratio;
    return e.engine.process(in, in_frames, channels, ratio, out_frames, sample_rate, out);
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