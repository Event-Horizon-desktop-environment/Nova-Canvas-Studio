#pragma once

// TimelineSelection — Qt-free selection state + math for the timeline
// (splitplan Phase 28). The widget owns a SelectionState and re-paints the
// scene from it; the model itself never touches Qt, so it is unit-testable from
// the headless seam (gui/tests) and scanned by scripts/check_qtdep.sh.
//
// Two plain functions (range membership, mate expansion) plus an owning
// SelectionState. Selection is an ordered, deduped id list; linked A/V mates are
// coalesced in on set() so the widget never has to reason about them elsewhere.

#include <cstdint>
#include <vector>

#include "canvas/core/timeline/model.hpp"

namespace canvas::gui {
namespace timeline_selection {

// All clips (video + audio) that overlap the frame range [lo, hi). Video-track
// clips come first, then audio — the order a rubber-band selection should show.
[[nodiscard]] std::vector<canvas::core::ClipId> clips_in_range(
    const canvas::core::Sequence& seq, int64_t lo, int64_t hi);

// Clips in the frame range [lo, hi) on tracks whose flat index lies in
// [min_track, max_track] (inclusive).  Track indices follow the widget
// convention: video tracks 0..v-1 (bottom-up), then audio tracks v..total-1.
[[nodiscard]] std::vector<canvas::core::ClipId> clips_in_range_and_tracks(
    const canvas::core::Sequence& seq, int64_t lo, int64_t hi,
    int min_track, int max_track);

// A user-clicked id set expanded with each id's linked A/V mate (linked clips
// always move/select together). Ids already present are not duplicated.
[[nodiscard]] std::vector<canvas::core::ClipId> expand_with_mates(
    const std::vector<canvas::core::ClipId>& ids, const canvas::core::Sequence& seq);

// Owned selection state. set() coalesces linked mates; pass a null Sequence to
// set the raw id list unchanged (defensive only — callers normally have one).
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

}  // namespace timeline_selection
}  // namespace canvas::gui