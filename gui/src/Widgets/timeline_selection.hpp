#pragma once

#include <cstdint>
#include <vector>

#include "canvas/core/timeline/model.hpp"

namespace canvas::gui {
namespace timeline_selection {

[[nodiscard]] std::vector<canvas::core::ClipId> clips_in_range(
    const canvas::core::Sequence& seq, int64_t lo, int64_t hi);

[[nodiscard]] std::vector<canvas::core::ClipId> clips_in_range_and_tracks(
    const canvas::core::Sequence& seq, int64_t lo, int64_t hi,
    int min_track, int max_track);

[[nodiscard]] std::vector<canvas::core::ClipId> expand_with_mates(
    const std::vector<canvas::core::ClipId>& ids, const canvas::core::Sequence& seq);

class SelectionState {
public:
    void set(const std::vector<canvas::core::ClipId>& ids, const canvas::core::Sequence* seq);
    void clear();

    [[nodiscard]] bool contains(canvas::core::ClipId id) const;
    [[nodiscard]] const std::vector<canvas::core::ClipId>& ids() const { return ids_; }
    [[nodiscard]] std::size_t size() const { return ids_.size(); }

private:
    std::vector<canvas::core::ClipId> ids_;
};

}
}
