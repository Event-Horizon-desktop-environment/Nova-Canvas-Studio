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

private:
    void evict_locked();

    mutable std::mutex mutex_;
    std::size_t max_bytes_;
    std::size_t bytes_ = 0;
    std::list<int64_t> lru_;
    std::unordered_map<int64_t, std::pair<VideoFramePtr, std::list<int64_t>::iterator>> map_;
};

}
