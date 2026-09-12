#pragma once

#include "canvas/core/timeline/model.hpp"

#include <array>
#include <memory>
#include <string>
#include <vector>

namespace canvas::core {

struct TrackSnapshot {
    Track::Kind kind = Track::Kind::Video;
    std::size_t index = 0;
    bool locked = false;
    bool muted = false;
    bool solo = false;
    float gain_db = 0.0f;
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

// `media_fps` is the source media's own frame rate. Placement derives the clip's
// timeline duration from the source window *time-based* (`tl = src * seq.fps /
// media.fps`), matching the renderer/playback source stride — so 60fps footage on
// a 30fps timeline spans its real duration, not twice it. Defaults to seq.fps
// (the historical frame-for-frame law) for callers without media context; fps==seq
// placements are identical either way.
std::unique_ptr<ICommand> place_clip(Sequence& seq, Track::Kind kind, std::size_t track_index,
                                     Clip clip, Placement mode, double media_fps = 0.0);
std::unique_ptr<ICommand> place_linked_clip(Sequence& seq, std::size_t video_track,
                                            std::size_t audio_track, Clip video, Clip audio,
                                            Placement mode, double media_fps = 0.0);
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
// A single clip destination for a batch move: the clip `id` ends on the track
// `kind`/`track_index` at `new_tl_in`. Track indices are PER-KIND (audio entries
// index into audio_tracks).
struct BatchMove {
    ClipId id = 0;
    Track::Kind kind = Track::Kind::Video;
    std::size_t track_index = 0;
    int64_t new_tl_in = 0;
};
// Atomically moves several clips at once (one undo command). All moved clips are
// extracted from their source tracks FIRST, then each is placed at its target, so
// the group never clips or consumes its own members the way sequential move_clip
// calls would; only stationary (non-dragged) clips get trimmed by a final
// overlap. Linked clips missing from `moves` follow their rep by the same tl_in
// delta, matching move_clip's mate semantics. Returns nullptr if `moves` is
// empty; entries whose source/destination track is locked or whose clip cannot
// be found are skipped.
std::unique_ptr<ICommand> move_clips_batch(Sequence& seq, const std::vector<BatchMove>& moves);
// Trims a clip's HEAD (left edge) to `new_tl_in`. `src_in` follows in lockstep
// so the pictured content moves with the edge; the edge can be dragged back to
// extend the clip but is clamped so it never goes below 0 or overlaps the track's
// left neighbor, and never pushes src_in below the source start. If the clip is
// linked, the mate's head trims by the same frame delta (clamped to its own
// limits so the pair stays mated). `media_frames` is the clip's media duration
// (total_frames) used to bound source-based clamps; pass 0 to allow no source
// extension. Returns nullptr if the clip is not found, the track is locked, or
// the requested position is already at the current edge.
std::unique_ptr<ICommand> trim_clip_head(Sequence& seq, Track::Kind kind, std::size_t track_index,
                                         ClipId id, int64_t new_tl_in, int64_t media_frames);
// Same as trim_clip_head but for the clip's TAIL (right edge) at `new_tl_out`;
// `src_out` follows in lockstep and can never exceed `media_frames` or overlap
// the track's right neighbor. This enables "regrow" after a blade+delete: the
// source window extends back into the deleted region.
std::unique_ptr<ICommand> trim_clip_tail(Sequence& seq, Track::Kind kind, std::size_t track_index,
                                         ClipId id, int64_t new_tl_out, int64_t media_frames);
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
// Sets the audio mix parameters (Volume in dB, Pan in [-1,1]) on a clip. If the
// clip is linked, the mate inherits the same values (both halves of an A/V pair
// share one loudness/position). Returns nullptr if the clip is not found.
std::unique_ptr<ICommand> set_clip_audio(Sequence& seq, Track::Kind kind,
                                         std::size_t track_index, ClipId id,
                                         float volume_db, float pan);
// Sets audio processing parameters (pitch, speed, EQ) on a clip. If the clip is
// linked, the mate inherits the same values. Returns nullptr if the clip is not
// found.
std::unique_ptr<ICommand> set_clip_audio_processing(Sequence& seq, Track::Kind kind,
                                                    std::size_t track_index, ClipId id,
                                                    float pitch_semitones, float pitch_cents,
                                                    float speed_factor, bool speed_enabled,
                                                    bool eq_enabled,
                                                    const std::array<Clip::EqBand, 6>& eq_bands);
// Sets the AI voice-isolation engine on a clip's audio. If the clip is linked,
// the mate inherits the same mode (both halves of an A/V pair share one audio
// treatment). Returns nullptr if the clip is not found.
std::unique_ptr<ICommand> set_clip_voice_isolation(Sequence& seq, Track::Kind kind,
                                                   std::size_t track_index, ClipId id,
                                                   VoiceIsolationMode mode);
// Sets a video clip's visual transform (Zoom scale_x/scale_y, pixel Position
// pos_x/pos_y, Rotation in degrees, Anchor offsets in pixels, and the flips).
// If the clip is linked, its mate inherits the same values (an A/V pair shares
// one transform; the audio half is a no-op visually). Values are clamped to the
// visual limits. Returns nullptr if the clip is not found. Does nothing audible
// regardless of the track kind (the fields are shared, not audio).
std::unique_ptr<ICommand> set_clip_transform(Sequence& seq, Track::Kind kind,
                                             std::size_t track_index, ClipId id,
                                             float scale_x, float scale_y,
                                             double pos_x, double pos_y,
                                             float rotation_deg,
                                             double anchor_dx, double anchor_dy,
                                             bool flip_h, bool flip_v);
// Sets a video clip's composite (opacity in [0,1] and the blend mode) with the
// same linked-mate propagation and clamping as set_clip_transform.
std::unique_ptr<ICommand> set_clip_composite(Sequence& seq, Track::Kind kind,
                                             std::size_t track_index, ClipId id,
                                             float opacity, BlendMode blend_mode);
// Replaces a clip's color grade (the node tree applied before the composite
// blit). If the clip is linked, the mate inherits the same graph (an A/V pair
// shares one grade; the audio half is a visual no-op). Passing an empty graph
// clears the grade. Returns nullptr if the clip is not found.
std::unique_ptr<ICommand> set_clip_grade(Sequence& seq, Track::Kind kind,
                                         std::size_t track_index, ClipId id,
                                         const grade_graph::GradeGraph& grade);
// Sets transition shaping on a clip's IN or OUT edge: ease amount, curve value,
// and the start/end ratio profile (all per-edge). If the clip is linked, the
// mate's corresponding edge inherits the values. Returns nullptr if the clip is
// not found.
std::unique_ptr<ICommand> set_clip_transition_curve(Sequence& seq, Track::Kind kind,
                                                    std::size_t track_index, ClipId id,
                                                    bool in_edge, float ease_amount,
                                                    float curve_value, int start_ratio,
                                                    int end_ratio);
// Sets clip metadata (tag, colour, comments, name). If the clip is linked, the
// mate inherits tag/colour/comments (name is per-clip). Returns nullptr if the
// clip is not found.
std::unique_ptr<ICommand> set_clip_metadata(Sequence& seq, Track::Kind kind,
                                            std::size_t track_index, ClipId id,
                                            Clip::ClipTag tag, uint8_t color,
                                            const std::string& comments,
                                            const std::string& name);
// Toggles a track's audio mixing flags. Each returns nullptr if the track
// index is out of range. These are discrete per-track settings; the model's
// `locked` flag (also toggled here) makes the track read-only in the timeline.
std::unique_ptr<ICommand> set_track_muted(Sequence& seq, Track::Kind kind,
                                          std::size_t track_index, bool muted);
std::unique_ptr<ICommand> set_track_solo(Sequence& seq, Track::Kind kind,
                                         std::size_t track_index, bool solo);
std::unique_ptr<ICommand> set_track_locked(Sequence& seq, Track::Kind kind,
                                           std::size_t track_index, bool locked);
// Sets an audio track's gain in dB (shared audio_mix law). Returns nullptr if
// the track index is out of range.
std::unique_ptr<ICommand> set_track_gain(Sequence& seq, Track::Kind kind,
                                         std::size_t track_index, float gain_db);

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
