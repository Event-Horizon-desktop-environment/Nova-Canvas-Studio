#include "canvas/core/media/frame_cache.hpp"

#include <algorithm>
#include <iterator>

namespace canvas::core {

VideoFramePtr FrameCache::get(const int64_t frame_number) {
    const std::lock_guard lock(mutex_);
    const auto it = map_.find(frame_number);
    if (it == map_.end()) return nullptr;
    lru_.splice(lru_.end(), lru_, it->second.second);
    return it->second.first;
}

void FrameCache::put(VideoFramePtr frame) {
    if (!frame || frame->bytes() > max_bytes_) return;
    const std::lock_guard lock(mutex_);
    if (map_.contains(frame->frame_number)) return;
    const int64_t number = frame->frame_number;
    bytes_ += frame->bytes();
    lru_.push_back(number);
    map_.emplace(number, std::make_pair(std::move(frame), std::prev(lru_.end())));
    evict_locked();
}

void FrameCache::evict_locked() {
    while (bytes_ > max_bytes_ && !lru_.empty()) {
        auto it = map_.find(lru_.front());
        lru_.pop_front();
        if (it == map_.end()) continue;
        bytes_ -= it->second.first->bytes();
        map_.erase(it);
    }
}

void FrameCache::clear() {
    const std::lock_guard lock(mutex_);
    map_.clear();
    lru_.clear();
    bytes_ = 0;
}

std::size_t FrameCache::size_bytes() const {
    const std::lock_guard lock(mutex_);
    return bytes_;
}

}
