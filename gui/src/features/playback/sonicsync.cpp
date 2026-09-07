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
    // Only a release (presenting the target frame) drops the hold. A paused seek
    // (begin_seek_hold is only called while playing) is mirrored by on_seek_paused,
    // which does not set hold_active_ so this is a no-op there.
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
    hold_released_ = true;  // audio may resume; caller re-anchors to hold_target_
    pending_reanchor_ = false;
}

void SonicSync::on_seek_paused(const std::int64_t target) {
    // Not playing: no audio to hold. Just note that we've moved so a later play()
    // must re-anchor audio to `target` rather than a stale pre-seek anchor.
    hold_active_ = false;
    hold_released_ = false;
    hold_target_ = target;
    pending_reanchor_ = true;
}

std::int64_t SonicSync::reconcile(std::int64_t want, const std::int64_t aud_seq,
                                  const std::int64_t video_lead,
                                  const std::int64_t total) const {
    if (total <= 0) return want;
    // The picture must never run more than `video_lead` frames past where the
    // speaker currently is: otherwise video races ahead of what's audible (the
    // runaway drop bug). Clamp the desired frame to the audible position + lead.
    if (aud_seq >= 0) {
        const std::int64_t ceiling = aud_seq + std::max<std::int64_t>(video_lead, 1);
        if (want > ceiling) {
            return std::clamp<std::int64_t>(ceiling, 0, total - 1);
        }
    }
    return std::clamp<std::int64_t>(want, 0, total - 1);
}

}  // namespace canvas::gui
