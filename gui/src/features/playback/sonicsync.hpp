#pragma once

#include <cstdint>
#include <chrono>

#include <canvas/core/project/project.hpp>

namespace canvas::gui {

class SonicSync {
public:
    void begin_seek_hold(std::int64_t target);

    void end_seek_hold();

    void on_seek_paused(std::int64_t target);

    struct HoldStats {
        std::uint64_t hold_count = 0;
        double hold_ms_min = 0.0;
        double hold_ms_max = 0.0;
        double hold_ms_avg = 0.0;
    };
    [[nodiscard]] HoldStats hold_stats() const
    {
        HoldStats out{hold_count_, hold_ms_min_, hold_ms_max_, 0.0};
        if (hold_count_ > 0) out.hold_ms_avg = hold_ms_sum_ / static_cast<double>(hold_count_);
        return out;
    }

    [[nodiscard]] bool seek_hold_active() const { return hold_active_ && !hold_released_; }

    [[nodiscard]] bool pending_reanchor() const { return pending_reanchor_; }
    void confirm_reanchor() { pending_reanchor_ = false; }

    std::int64_t reconcile(std::int64_t want, std::int64_t aud_seq,
                           std::int64_t video_lead, std::int64_t total) const;

private:
    bool hold_active_ = false;
    bool hold_released_ = false;
    std::int64_t hold_target_ = 0;
    bool pending_reanchor_ = false;
    std::chrono::steady_clock::time_point hold_start_{};
    std::uint64_t hold_count_ = 0;
    double hold_ms_sum_ = 0.0;
    double hold_ms_min_ = 0.0;
    double hold_ms_max_ = 0.0;
};

}