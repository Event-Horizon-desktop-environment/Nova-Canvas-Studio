#include "Widgets/timeline_selection.hpp"

#include <algorithm>
#include <vector>

namespace canvas::gui {
namespace timeline_selection {

std::vector<canvas::core::ClipId> clips_in_range(const canvas::core::Sequence& seq,
                                             const int64_t lo, const int64_t hi) {
    std::vector<canvas::core::ClipId> ids;
    const auto scan = [&](const canvas::core::Track& track) {
        for (const auto& c : track.clips)
            if (c.tl_out > lo && c.tl_in < hi) ids.push_back(c.id);
    };
    for (const auto& t : seq.video_tracks) scan(t);
    for (const auto& t : seq.audio_tracks) scan(t);
    return ids;
}

std::vector<canvas::core::ClipId> clips_in_range_and_tracks(const canvas::core::Sequence& seq,
                                                        const int64_t lo, const int64_t hi,
                                                        const int min_track, const int max_track) {
    std::vector<canvas::core::ClipId> ids;
    if (max_track < min_track) return ids;
    const auto scan = [&](const canvas::core::Track& track) {
        for (const auto& c : track.clips)
            if (c.tl_out > lo && c.tl_in < hi) ids.push_back(c.id);
    };
    const int v = static_cast<int>(seq.video_tracks.size());
    for (int i = 0; i < v; ++i)
        if (i >= min_track && i <= max_track) scan(seq.video_tracks[i]);
    for (int i = 0; i < static_cast<int>(seq.audio_tracks.size()); ++i)
        if (i + v >= min_track && i + v <= max_track) scan(seq.audio_tracks[i]);
    return ids;
}

std::vector<canvas::core::ClipId> expand_with_mates(
    const std::vector<canvas::core::ClipId>& ids, const canvas::core::Sequence& seq) {
    std::vector<canvas::core::ClipId> out = ids;
    const auto add_mate = [&](const canvas::core::Clip* c) {
        if (c && c->is_linked() &&
            std::find(out.begin(), out.end(), c->linked_id) == out.end())
            out.push_back(c->linked_id);
    };
    for (const canvas::core::ClipId id : ids) {
        for (auto& t : seq.video_tracks)
            if (const canvas::core::Clip* c = t.clip_with_id(id)) { add_mate(c); break; }
        for (auto& t : seq.audio_tracks)
            if (const canvas::core::Clip* c = t.clip_with_id(id)) { add_mate(c); break; }
    }
    return out;
}

void SelectionState::set(const std::vector<canvas::core::ClipId>& ids,
                         const canvas::core::Sequence* seq) {
    ids_ = seq ? expand_with_mates(ids, *seq) : ids;
}

void SelectionState::clear() { ids_.clear(); }

bool SelectionState::contains(const canvas::core::ClipId id) const {
    return std::find(ids_.begin(), ids_.end(), id) != ids_.end();
}

}  // namespace timeline_selection
}  // namespace canvas::gui