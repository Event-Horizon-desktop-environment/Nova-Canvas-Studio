#include "sonicsync.hpp"

#include <algorithm>

namespace canvas::gui {

void SonicSync::begin_seek_hold(const std::int64_t target) {
    hold_active_ = true;
    hold_released_ = false;
    hold_target_ = target;
    pending_reanchor_ = true;
    hold_start_ = std::chrono::steady_clock::now();
}

void SonicSync::end_seek_hold() {
    if (!hold_active_) return;
    const double hold_ms = std::chrono::duration<double, std::milli>(
                               std::chrono::steady_clock::now() - hold_start_).count();
    if (hold_count_ == 0) {
        hold_ms_min_ = hold_ms_max_ = hold_ms;
    } else {
        hold_ms_min_ = std::min(hold_ms_min_, hold_ms);
        hold_ms_max_ = std::max(hold_ms_max_, hold_ms);
    }
    ++hold_count_;
    hold_ms_sum_ += hold_ms;
    hold_released_ = true;
    pending_reanchor_ = false;
}

void SonicSync::on_seek_paused(const std::int64_t target) {
    hold_active_ = false;
    hold_released_ = false;
    hold_target_ = target;
    pending_reanchor_ = true;
}

std::int64_t SonicSync::reconcile(std::int64_t want, const std::int64_t aud_seq,
                                  const std::int64_t video_lead,
                                  const std::int64_t total) const {
    if (total <= 0) return want;
    if (aud_seq >= 0) {
        const std::int64_t ceiling = aud_seq + std::max<std::int64_t>(video_lead, 1);
        if (want > ceiling) {
            return std::clamp<std::int64_t>(ceiling, 0, total - 1);
        }
    }
    return std::clamp<std::int64_t>(want, 0, total - 1);
}

}
