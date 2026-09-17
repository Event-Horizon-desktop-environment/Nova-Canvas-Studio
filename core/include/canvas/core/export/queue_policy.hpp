#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

namespace canvas::core::queue_policy {

struct Candidate {
    int priority = 0;
    std::uint64_t order = 0;
    bool queued = true;
};

[[nodiscard]] inline int next_candidate(const std::span<const Candidate> candidates) noexcept {
    int best = -1;
    for (std::size_t i = 0; i < candidates.size(); ++i) {
        if (!candidates[i].queued) continue;
        if (best < 0) {
            best = static_cast<int>(i);
            continue;
        }
        const Candidate& b = candidates[static_cast<std::size_t>(best)];
        const Candidate& c = candidates[i];
        if (c.priority > b.priority ||
            (c.priority == b.priority && c.order < b.order)) {
            best = static_cast<int>(i);
        }
    }
    return best;
}

}
