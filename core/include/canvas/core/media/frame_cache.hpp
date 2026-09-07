#pragma once

#include "canvas/core/media/frame.hpp"

#include <cstddef>
#include <cstdint>
#include <list>
#include <mutex>
#include <unordered_map>
#include <utility>

namespace canvas::core {

class FrameCache {
public:
    explicit FrameCache(std::size_t max_bytes = 512ull * 1024 * 1024) noexcept
        : max_bytes_(max_bytes) {}

    VideoFramePtr get(int64_t frame_number);
    void put(VideoFramePtr frame);
    void clear();
    [[nodiscard]] std::size_t size_bytes() const;

    // Cumulative accounting since construction (or the last clear()), for
    // playback health logs: how well the budget is being reused (hit rate) and
    // how often a full slot silently evict-reloads (an eviction storm shows up
    // as misses climbing while bytes stay pinned at the cap).
    struct Stats {
        std::uint64_t hits = 0;
        std::uint64_t misses = 0;
        std::uint64_t evictions = 0;
        std::size_t bytes = 0;
        std::size_t max_bytes = 0;
    };
    [[nodiscard]] Stats stats() const;

private:
    void evict_locked();

    mutable std::mutex mutex_;
    std::size_t max_bytes_;
    std::size_t bytes_ = 0;
    std::list<int64_t> lru_;
    std::unordered_map<int64_t, std::pair<VideoFramePtr, std::list<int64_t>::iterator>> map_;
    std::uint64_t hits_ = 0;
    std::uint64_t misses_ = 0;
    std::uint64_t evictions_ = 0;
};

}
