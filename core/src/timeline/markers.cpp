#include "canvas/core/timeline/markers.hpp"

namespace canvas::core::markers {

std::vector<Chapter> chapters_from(const Sequence& seq) {
    std::vector<Chapter> out;
    if (seq.fps <= 0.0) return out;
    out.reserve(seq.bookmarks.size());
    for (const auto& b : seq.bookmarks) {
        Chapter c;
        c.seconds = static_cast<double>(b.frame) / seq.fps;
        c.label = b.label;
        if (c.label.empty()) c.label = "Chapter " + std::to_string(out.size() + 1);
        out.push_back(std::move(c));
    }
    return out;
}

}  // namespace canvas::core::markers