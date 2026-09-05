#pragma once

#include "canvas/core/timeline/model.hpp"

#include <memory>
#include <string>
#include <vector>

namespace canvas::core {

struct TrackSnapshot {
    Track::Kind kind = Track::Kind::Video;
    std::size_t index = 0;
    std::vector<Clip> clips;
};

class ICommand {
public:
    virtual ~ICommand() = default;
    virtual void redo(Sequence& seq) = 0;
    virtual void undo(Sequence& seq) = 0;
    [[nodiscard]] virtual const std::string& name() const noexcept = 0;
};

class EditCommand final : public ICommand {
public:
    EditCommand(std::string name, std::vector<TrackSnapshot> before, std::vector<TrackSnapshot> after);

    void redo(Sequence& seq) override;
    void undo(Sequence& seq) override;
    [[nodiscard]] const std::string& name() const noexcept override { return name_; }

private:
    static void apply(Sequence& seq, const std::vector<TrackSnapshot>& state);

    std::string name_;
    std::vector<TrackSnapshot> before_;
    std::vector<TrackSnapshot> after_;
};

enum class Placement { Overwrite, Insert, AppendAtEnd, PlaceOnTop };

std::unique_ptr<ICommand> place_clip(Sequence& seq, Track::Kind kind, std::size_t track_index,
                                     Clip clip, Placement mode);
std::unique_ptr<ICommand> place_linked_clip(Sequence& seq, std::size_t video_track,
                                            std::size_t audio_track, Clip video, Clip audio,
                                            Placement mode);
std::unique_ptr<ICommand> unlink_clip(Sequence& seq, Track::Kind kind, std::size_t track_index,
                                      ClipId id);
// Links an unlinked clip to an unlinked clip of the opposite kind whose time
// range overlaps the source clip (preferring the best overlap). Returns nullptr
// if no compatible mate exists or the clip is already linked.
std::unique_ptr<ICommand> link_clip(Sequence& seq, Track::Kind kind, std::size_t track_index,
                                    ClipId id);
std::unique_ptr<ICommand> lift_range(Sequence& seq, Track::Kind kind, std::size_t track_index,
                                     int64_t in, int64_t out);
std::unique_ptr<ICommand> ripple_delete_range(Sequence& seq, Track::Kind kind,
                                              std::size_t track_index, int64_t in, int64_t out);
std::unique_ptr<ICommand> blade_at(Sequence& seq, Track::Kind kind, std::size_t track_index,
                                   int64_t pos);
// Cuts the clip under `pos` AND its linked mate (if any) at the same position.
std::unique_ptr<ICommand> blade_linked_at(Sequence& seq, Track::Kind kind, std::size_t track_index,
                                          int64_t pos);
// Deletes a single clip (removing it), *without* closing the gap. If the clip
// is linked, its linked mate is also removed. Returns the command or nullptr.
std::unique_ptr<ICommand> lift_clip(Sequence& seq, Track::Kind kind, std::size_t track_index,
                                    ClipId id);
// Ripple-deletes a single clip, closing the gap by shifting later clips left.
// If the clip is linked, its linked mate is also removed (without rippling the
// mate's track unless the mate is on a different kind of track).
std::unique_ptr<ICommand> ripple_delete_clip(Sequence& seq, Track::Kind kind,
                                             std::size_t track_index, ClipId id);
std::unique_ptr<ICommand> move_clip(Sequence& seq, Track::Kind src_kind, std::size_t src_track,
                                    ClipId id, Track::Kind dst_kind, std::size_t dst_track,
                                    int64_t new_tl_in);
// Auto-creates a new topmost video track (index 0) and a new topmost audio track
// (index 0), then moves the clip `id` onto the new video track at `new_tl_in`.
// If the clip is linked, its linked mate moves to the new audio track, keeping
// A/V sync (the mate's time offset relative to the primary is preserved). All of
// this happens in a *single* undoable command; undoing restores the clips to
// their original tracks (the new tracks themselves are left in place, matching
// the non-undoable add/remove-track behavior). Returns nullptr if the clip is
// not found.
std::unique_ptr<ICommand> create_top_track_move(Sequence& seq, ClipId id, int64_t new_tl_in);
// Sets the enabled/mute flag on a clip (and its linked mate, if any) so both
// halves toggle together in a single undoable step. Returns nullptr if the clip
// is not found.
std::unique_ptr<ICommand> set_clip_enabled(Sequence& seq, Track::Kind kind, std::size_t track_index,
                                           ClipId id, bool enabled);
// Sets the transition applied at a clip's OUT boundary (type + duration in
// frames). If the clip is linked, the mate's transition is set to match, so a
// video dissolve also crossfades its linked audio. Returns nullptr if the clip
// is not found.
std::unique_ptr<ICommand> set_clip_transition(Sequence& seq, Track::Kind kind,
                                              std::size_t track_index, ClipId id,
                                              TransitionType type, int64_t duration);
// Clears the transition on a clip (and its linked mate). Returns nullptr if the
// clip is not found.
std::unique_ptr<ICommand> clear_clip_transition(Sequence& seq, Track::Kind kind,
                                                std::size_t track_index, ClipId id);
// Sets the transition applied at a clip's IN (leading) boundary (type + duration
// in frames), fading the clip in over its first frames. Independent of the OUT
// transition. If the clip is linked, the mate's IN transition is set to match.
// Returns nullptr if the clip is not found.
std::unique_ptr<ICommand> set_clip_transition_in(Sequence& seq, Track::Kind kind,
                                                 std::size_t track_index, ClipId id,
                                                 TransitionType type, int64_t duration);
// Clears the IN transition on a clip (and its linked mate). Returns nullptr if
// the clip is not found.
std::unique_ptr<ICommand> clear_clip_transition_in(Sequence& seq, Track::Kind kind,
                                                   std::size_t track_index, ClipId id);
// Removes the edit point (cut) where the incoming clip `B` (with B.tl_in ==
// A.tl_out) joins the outgoing clip `A` (identified by `out_id`), merging the
// two adjacent same-media clips into a single continuous clip spanning
// [A.tl_in, B.tl_out). Returns nullptr if the clips are not adjacent on the same
// track or do not share the same media (i.e. the edit is not a genuine "through"
// edit that can be joined).
std::unique_ptr<ICommand> delete_through_edit(Sequence& seq, Track::Kind kind,
                                              std::size_t track_index, ClipId out_id);

class UndoStack {
public:
    void record(std::unique_ptr<ICommand> command);
    bool undo(Sequence& seq);
    bool redo(Sequence& seq);
    void clear();
    [[nodiscard]] bool can_undo() const noexcept { return !undo_.empty(); }
    [[nodiscard]] bool can_redo() const noexcept { return !redo_.empty(); }
    [[nodiscard]] const std::string& next_undo_name() const noexcept { return undo_.back()->name(); }
    [[nodiscard]] const std::string& next_redo_name() const noexcept { return redo_.back()->name(); }
    [[nodiscard]] std::size_t count() const noexcept { return undo_.size(); }

private:
    std::vector<std::unique_ptr<ICommand>> undo_;
    std::vector<std::unique_ptr<ICommand>> redo_;
};

}
