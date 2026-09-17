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
    bool collapsed = false;
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

class GroupCommand final : public ICommand {
public:
    GroupCommand(std::string name, std::vector<std::unique_ptr<ICommand>> children);

    void redo(Sequence& seq) override;
    void undo(Sequence& seq) override;
    [[nodiscard]] const std::string& name() const noexcept override { return name_; }

private:
    std::string name_;
    std::vector<std::unique_ptr<ICommand>> children_;
};

class TrackListCommand final : public ICommand {
public:
    TrackListCommand(std::string name, Track::Kind kind, std::vector<Track> before,
                     std::vector<Track> after);

    void redo(Sequence& seq) override;
    void undo(Sequence& seq) override;
    [[nodiscard]] const std::string& name() const noexcept override { return name_; }

private:
    std::string name_;
    Track::Kind kind_;
    std::vector<Track> before_;
    std::vector<Track> after_;
};

enum class Placement { Overwrite, Insert, AppendAtEnd, PlaceOnTop };

std::unique_ptr<ICommand> place_clip(Sequence& seq, Track::Kind kind, std::size_t track_index,
                                     Clip clip, Placement mode, double media_fps = 0.0);
std::unique_ptr<ICommand> place_linked_clip(Sequence& seq, std::size_t video_track,
                                            std::size_t audio_track, Clip video, Clip audio,
                                            Placement mode, double media_fps = 0.0);
std::unique_ptr<ICommand> unlink_clip(Sequence& seq, Track::Kind kind, std::size_t track_index,
                                      ClipId id);
std::unique_ptr<ICommand> link_clip(Sequence& seq, Track::Kind kind, std::size_t track_index,
                                    ClipId id);
std::unique_ptr<ICommand> lift_range(Sequence& seq, Track::Kind kind, std::size_t track_index,
                                     int64_t in, int64_t out);
std::unique_ptr<ICommand> ripple_delete_range(Sequence& seq, Track::Kind kind,
                                              std::size_t track_index, int64_t in, int64_t out);
std::unique_ptr<ICommand> blade_at(Sequence& seq, Track::Kind kind, std::size_t track_index,
                                   int64_t pos);
std::unique_ptr<ICommand> blade_linked_at(Sequence& seq, Track::Kind kind, std::size_t track_index,
                                          int64_t pos);
std::unique_ptr<ICommand> lift_clip(Sequence& seq, Track::Kind kind, std::size_t track_index,
                                    ClipId id);
std::unique_ptr<ICommand> ripple_delete_clip(Sequence& seq, Track::Kind kind,
                                             std::size_t track_index, ClipId id);
std::unique_ptr<ICommand> move_clip(Sequence& seq, Track::Kind src_kind, std::size_t src_track,
                                    ClipId id, Track::Kind dst_kind, std::size_t dst_track,
                                    int64_t new_tl_in);
struct BatchMove {
    ClipId id = 0;
    Track::Kind kind = Track::Kind::Video;
    std::size_t track_index = 0;
    int64_t new_tl_in = 0;
};
std::unique_ptr<ICommand> move_clips_batch(Sequence& seq, const std::vector<BatchMove>& moves);
std::unique_ptr<ICommand> trim_clip_head(Sequence& seq, Track::Kind kind, std::size_t track_index,
                                         ClipId id, int64_t new_tl_in, int64_t media_frames);
std::unique_ptr<ICommand> trim_clip_tail(Sequence& seq, Track::Kind kind, std::size_t track_index,
                                         ClipId id, int64_t new_tl_out, int64_t media_frames);
std::unique_ptr<ICommand> create_top_track_move(Sequence& seq, ClipId id, int64_t new_tl_in);
std::unique_ptr<ICommand> set_clip_enabled(Sequence& seq, Track::Kind kind, std::size_t track_index,
                                           ClipId id, bool enabled);
std::unique_ptr<ICommand> set_clip_transition(Sequence& seq, Track::Kind kind,
                                              std::size_t track_index, ClipId id,
                                              TransitionType type, int64_t duration);
std::unique_ptr<ICommand> clear_clip_transition(Sequence& seq, Track::Kind kind,
                                                std::size_t track_index, ClipId id);
std::unique_ptr<ICommand> set_clip_transition_in(Sequence& seq, Track::Kind kind,
                                                 std::size_t track_index, ClipId id,
                                                 TransitionType type, int64_t duration);
std::unique_ptr<ICommand> clear_clip_transition_in(Sequence& seq, Track::Kind kind,
                                                   std::size_t track_index, ClipId id);
std::unique_ptr<ICommand> delete_through_edit(Sequence& seq, Track::Kind kind,
                                              std::size_t track_index, ClipId out_id);
std::unique_ptr<ICommand> set_clip_audio(Sequence& seq, Track::Kind kind,
                                         std::size_t track_index, ClipId id,
                                         float volume_db, float pan);
std::unique_ptr<ICommand> set_clip_audio_processing(Sequence& seq, Track::Kind kind,
                                                    std::size_t track_index, ClipId id,
                                                    float pitch_semitones, float pitch_cents,
                                                    float speed_factor, bool speed_enabled,
                                                    bool eq_enabled,
                                                    const std::array<Clip::EqBand, 6>& eq_bands);
std::unique_ptr<ICommand> set_clip_voice_isolation(Sequence& seq, Track::Kind kind,
                                                   std::size_t track_index, ClipId id,
                                                   VoiceIsolationMode mode);
std::unique_ptr<ICommand> set_clip_transform(Sequence& seq, Track::Kind kind,
                                             std::size_t track_index, ClipId id,
                                             float scale_x, float scale_y,
                                             double pos_x, double pos_y,
                                             float rotation_deg,
                                             double anchor_dx, double anchor_dy,
                                             bool flip_h, bool flip_v);
std::unique_ptr<ICommand> set_clip_composite(Sequence& seq, Track::Kind kind,
                                             std::size_t track_index, ClipId id,
                                             float opacity, BlendMode blend_mode);
std::unique_ptr<ICommand> set_clip_title(Sequence& seq, Track::Kind kind,
                                         std::size_t track_index, ClipId id,
                                         const Clip::Title& title);
std::unique_ptr<ICommand> set_clip_grade(Sequence& seq, Track::Kind kind,
                                         std::size_t track_index, ClipId id,
                                         const grade_graph::GradeGraph& grade);
std::unique_ptr<ICommand> set_clip_transition_curve(Sequence& seq, Track::Kind kind,
                                                    std::size_t track_index, ClipId id,
                                                    bool in_edge, float ease_amount,
                                                    float curve_value, int start_ratio,
                                                    int end_ratio);
std::unique_ptr<ICommand> set_clip_metadata(Sequence& seq, Track::Kind kind,
                                            std::size_t track_index, ClipId id,
                                            Clip::ClipTag tag, uint8_t color,
                                            const std::string& comments,
                                            const std::string& name);
std::unique_ptr<ICommand> set_track_muted(Sequence& seq, Track::Kind kind,
                                          std::size_t track_index, bool muted);
std::unique_ptr<ICommand> set_track_solo(Sequence& seq, Track::Kind kind,
                                         std::size_t track_index, bool solo);
std::unique_ptr<ICommand> set_track_locked(Sequence& seq, Track::Kind kind,
                                           std::size_t track_index, bool locked);
std::unique_ptr<ICommand> set_track_gain(Sequence& seq, Track::Kind kind,
                                         std::size_t track_index, float gain_db);

std::unique_ptr<ICommand> set_track_collapsed(Sequence& seq, Track::Kind kind,
                                              std::size_t track_index, bool collapsed);

std::unique_ptr<ICommand> set_all_tracks_collapsed(Sequence& seq, bool collapsed);

std::unique_ptr<ICommand> insert_track(Sequence& seq, Track::Kind kind, std::size_t index,
                                       const std::string& name = "");

std::unique_ptr<ICommand> remove_track(Sequence& seq, Track::Kind kind, std::size_t index);

std::unique_ptr<ICommand> rename_track(Sequence& seq, Track::Kind kind, std::size_t index,
                                       const std::string& name);

std::unique_ptr<ICommand> move_track(Sequence& seq, Track::Kind kind, std::size_t from,
                                     std::size_t to);

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
