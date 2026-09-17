#pragma once

#include "canvas/core/util/log.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <string>

namespace canvas::core::log {

class RenderTelemetry {
public:
    RenderTelemetry() = default;

    void note_gpu_attempt(bool landed, int reason, double total_ms, double decode_ms);

    void note_fast();
    void note_cpu(double comp_ms);
    void note_alloc_miss();
    void note_stall();
    void note_resize(double ms);

    void note_encode(double ms);
    void note_audio(double ms);
    void set_progress(std::int64_t done, std::int64_t total);
    void observe_queue(std::size_t depth);
    void note_pool_stalls(std::uint64_t stalls);

    void tick();

    void flush();

private:
    struct Acc {
        std::int64_t n = 0;
        double sum_ms = 0.0;
        double mean() const { return n > 0 ? sum_ms / static_cast<double>(n) : 0.0; }
        void add(double ms) {
            ++n;
            sum_ms += ms;
        }
        void reset() {
            n = 0;
            sum_ms = 0.0;
        }
    };

    void emit_locked(double fps, double eta_s, double comp_avg, double audio_avg,
                     double enc_avg, double since_s);

    std::int64_t fast_ = 0;
    std::int64_t cpu_ = 0;
    std::int64_t stalls_ = 0;
    std::int64_t alloc_miss_ = 0;
    std::uint64_t pool_stalls_ = 0;
    std::size_t qmax_ = 0;
    Acc comp_, audio_, enc_, gpu_ms_, decode_ms_, resize_ms_;

    std::int64_t gpu_attempts_ = 0;
    std::int64_t gpu_landed_ = 0;
    std::int64_t gpu_reasons_[5] = {0, 0, 0, 0, 0};

    std::int64_t done_ = 0;
    std::int64_t total_ = 1;
    std::int64_t done_at_last_emit_ = 0;
    std::chrono::steady_clock::time_point start_{std::chrono::steady_clock::now()};
    std::chrono::steady_clock::time_point last_emit_{};
    std::mutex mu_;
};

inline void RenderTelemetry::note_gpu_attempt(bool landed, int reason, double total_ms,
                                              double decode_ms) {
    std::lock_guard<std::mutex> lk(mu_);
    ++gpu_attempts_;
    gpu_ms_.add(total_ms);
    decode_ms_.add(decode_ms);
    if (landed) {
        ++gpu_landed_;
    } else if (reason >= 0 && reason < 5) {
        ++gpu_reasons_[reason];
    }
}

inline void RenderTelemetry::note_fast() {
    std::lock_guard<std::mutex> lk(mu_);
    ++fast_;
}

inline void RenderTelemetry::note_cpu(double comp_ms) {
    std::lock_guard<std::mutex> lk(mu_);
    ++cpu_;
    comp_.add(comp_ms);
}

inline void RenderTelemetry::note_alloc_miss() {
    std::lock_guard<std::mutex> lk(mu_);
    ++alloc_miss_;
}

inline void RenderTelemetry::note_stall() {
    std::lock_guard<std::mutex> lk(mu_);
    ++stalls_;
}

inline void RenderTelemetry::note_resize(double ms) {
    std::lock_guard<std::mutex> lk(mu_);
    resize_ms_.add(ms);
}

inline void RenderTelemetry::note_encode(double ms) {
    std::lock_guard<std::mutex> lk(mu_);
    enc_.add(ms);
}

inline void RenderTelemetry::note_audio(double ms) {
    std::lock_guard<std::mutex> lk(mu_);
    audio_.add(ms);
}

inline void RenderTelemetry::set_progress(std::int64_t done, std::int64_t total) {
    std::lock_guard<std::mutex> lk(mu_);
    done_ = done;
    total_ = total > 0 ? total : 1;
}

inline void RenderTelemetry::observe_queue(std::size_t depth) {
    std::lock_guard<std::mutex> lk(mu_);
    if (depth > qmax_) qmax_ = depth;
}

inline void RenderTelemetry::note_pool_stalls(std::uint64_t stalls) {
    std::lock_guard<std::mutex> lk(mu_);
    pool_stalls_ = stalls;
}

inline void RenderTelemetry::emit_locked(double fps, double eta_s, double comp_avg,
                                         double audio_avg, double enc_avg, double since_s) {
    const std::int64_t win_total = fast_ + cpu_;
    const double fast_pct =
        win_total > 0 ? 100.0 * static_cast<double>(fast_) / static_cast<double>(win_total) : 0.0;
    const std::int64_t gpu_bailed = gpu_attempts_ - gpu_landed_;
    ::canvas::core::log::log_info(
        "[render] frame=%lld/%lld pct=%.1f%% fps=%.2f eta_s=%.0f stalls=%lld "
        "fast=%.1f%% (gpu=%lld cpu=%lld) comp_avg_ms=%.1f audio_avg_ms=%.1f "
        "enc_avg_ms=%.1f alloc_miss=%lld qmax=%zu pool_stalls=%llu elaps_s=%.0f",
        static_cast<long long>(done_), static_cast<long long>(total_), fps > 0
            ? 100.0 * static_cast<double>(done_) / static_cast<double>(total_)
            : 0.0,
        fps, eta_s, static_cast<long long>(stalls_), fast_pct,
        static_cast<long long>(fast_), static_cast<long long>(cpu_), comp_avg, audio_avg,
        enc_avg, static_cast<long long>(alloc_miss_), qmax_,
        static_cast<unsigned long long>(pool_stalls_), since_s);
    ::canvas::core::log::log_info(
        "[gpu] fastpath n=%lld avg_ms=%.2f decode_ms=%.2f resize_ms=%.2f "
        "landed=%lld bailed=%lld reasons={%lld overlap, %lld fade_nodec, %lld open-fail, "
        "%lld decode-fail, %lld other}",
        static_cast<long long>(gpu_attempts_), gpu_ms_.mean(), decode_ms_.mean(),
        resize_ms_.mean(), static_cast<long long>(gpu_landed_),
        static_cast<long long>(gpu_bailed), static_cast<long long>(gpu_reasons_[0]),
        static_cast<long long>(gpu_reasons_[1]), static_cast<long long>(gpu_reasons_[2]),
        static_cast<long long>(gpu_reasons_[3]), static_cast<long long>(gpu_reasons_[4]));
}

inline void RenderTelemetry::tick() {
    std::lock_guard<std::mutex> lk(mu_);
    const auto now = std::chrono::steady_clock::now();
    if (last_emit_ != std::chrono::steady_clock::time_point{}) {
        const double since_s = std::chrono::duration<double>(now - last_emit_).count();
        if (since_s < 1.0) return;
        const double fps = (done_ - done_at_last_emit_) > 0 && since_s > 0
            ? static_cast<double>(done_ - done_at_last_emit_) / since_s
            : 0.0;
        const std::int64_t rem = total_ - std::min<std::int64_t>(done_, total_);
        const double eta_s = fps > 0 ? static_cast<double>(rem) / fps : 0.0;
        const double elaps_s =
            std::chrono::duration<double>(now - start_).count();
        emit_locked(fps, eta_s, comp_.mean(), audio_.mean(), enc_.mean(), elaps_s);
    }
    last_emit_ = now;
    done_at_last_emit_ = done_;
    fast_ = cpu_ = stalls_ = alloc_miss_ = 0;
    pool_stalls_ = 0;
    qmax_ = 0;
    comp_.reset();
    audio_.reset();
    enc_.reset();
    gpu_ms_.reset();
    decode_ms_.reset();
    resize_ms_.reset();
    gpu_attempts_ = gpu_landed_ = 0;
    gpu_reasons_[0] = gpu_reasons_[1] = gpu_reasons_[2] = gpu_reasons_[3] = gpu_reasons_[4] = 0;
}

inline void RenderTelemetry::flush() {
    std::lock_guard<std::mutex> lk(mu_);
    if (gpu_attempts_ == 0 && fast_ == 0 && cpu_ == 0) return;
    const auto now = std::chrono::steady_clock::now();
    const double since_s = std::chrono::duration<double>(now - start_).count();
    emit_locked(0.0, 0.0, comp_.mean(), audio_.mean(), enc_.mean(), since_s);
    last_emit_ = now;
    done_at_last_emit_ = done_;
    fast_ = cpu_ = stalls_ = alloc_miss_ = 0;
    qmax_ = 0;
    comp_.reset();
    audio_.reset();
    enc_.reset();
    gpu_ms_.reset();
    decode_ms_.reset();
    resize_ms_.reset();
    gpu_attempts_ = gpu_landed_ = 0;
    gpu_reasons_[0] = gpu_reasons_[1] = gpu_reasons_[2] = gpu_reasons_[3] = gpu_reasons_[4] = 0;
}

}
