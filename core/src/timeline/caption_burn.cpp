#include "canvas/core/timeline/caption_burn.hpp"

#include <algorithm>
#include <cmath>

namespace canvas::core::caption_burn {

namespace {

int64_t ms_to_frames(const int64_t ms, const double fps) {
    return static_cast<int64_t>(std::llround(static_cast<double>(ms) * fps / 1000.0));
}

}

std::vector<Window> windows_from(const std::vector<captions::Caption>& caps, const double fps) {
    const double rate = fps > 0.0 ? fps : 30.0;
    std::vector<Window> out;
    out.reserve(caps.size());
    for (const auto& c : caps) {
        Window w;
        w.start = ms_to_frames(c.start_ms, rate);
        w.end = ms_to_frames(c.end_ms, rate);
        if (w.end < w.start) w.end = w.start;
        w.text = c.text;
        out.push_back(std::move(w));
    }
    return out;
}

int active_at(const std::vector<Window>& windows, const int64_t tl_frame) noexcept {
    int best = -1;
    for (std::size_t i = 0; i < windows.size(); ++i) {
        const Window& w = windows[i];
        if (tl_frame < w.start || tl_frame >= w.end) continue;
        if (best < 0) {
            best = static_cast<int>(i);
            continue;
        }
        const Window& b = windows[static_cast<std::size_t>(best)];
        if (w.start > b.start || (w.start == b.start && w.end > b.end) ||
            (w.start == b.start && w.end == b.end && i > static_cast<std::size_t>(best))) {
            best = static_cast<int>(i);
        }
    }
    return best;
}

std::string text_at(const std::vector<Window>& windows, const int64_t tl_frame) {
    const int idx = active_at(windows, tl_frame);
    return idx < 0 ? std::string() : windows[static_cast<std::size_t>(idx)].text;
}

}
