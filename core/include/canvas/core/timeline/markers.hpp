#pragma once

// Marker/range → chapter mapping LAW (Qt-free). The deliver pipeline muxes
// containers' chapter tables (MP4/WebM/MKV) from the sequence's markers; this
// module is the one place that derivation lives so the exporter and the tests
// agree. Point markers (`tl_out == 0`) become a chapter at their frame; named
// ranges (`is_range()`) become a chapter at their START frame. Chapters come
// out in timeline order (bookmarks are kept sorted by start frame).

#include "canvas/core/timeline/model.hpp"

#include <string>
#include <vector>

namespace canvas::core::markers {

struct Chapter {
    double seconds = 0.0;  // timeline position, seconds
    std::string label;
};

// One chapter per bookmark, in timeline order. An empty label falls back to
// "Chapter N" (1-based) so exported chapter menus are never blank. A sequence
// with fps <= 0 yields an empty table (no meaningful timebase).
[[nodiscard]] std::vector<Chapter> chapters_from(const Sequence& seq);

}  // namespace canvas::core::markers