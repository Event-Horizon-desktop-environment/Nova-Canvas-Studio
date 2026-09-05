#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace canvas::core {

using MediaId = int;
using ClipId = uint64_t;

// The kind of transition applied at a clip's OUT boundary (the trailing edge
// of an outgoing clip, blending into the clip that starts exactly where this
// one ends). AudioFade is the audio crossfade family (see sub type).
enum class TransitionType {
    None = 0,
    CrossDissolve,
    DipToBlack,
    FadeOut,
    FadeIn,
    WipeLeft,
    WipeRight,
    WipeUp,
    WipeDown,
    AudioFadeConstantGain,
    AudioFadeConstantPower,
    AudioFadeExponential,
};

[[nodiscard]] inline bool is_audio_transition(const TransitionType t) noexcept {
    return t >= TransitionType::AudioFadeConstantGain;
}

struct Clip {
    ClipId id = 0;
    MediaId media = -1;
    int64_t tl_in = 0;
    int64_t tl_out = 0;
    int64_t src_in = 0;
    int64_t src_out = 0;
    std::string name;
    ClipId linked_id = 0;
    bool enabled = true;

    // Audio/video transition at this clip's OUT boundary. `transition_out` is
    // the effect kind; `transition_out_duration` is its length in frames (0 =
    // no transition). Stored per-clip so the existing track-snapshot undo
    // machinery restores it automatically.
    TransitionType transition_out = TransitionType::None;
    int64_t transition_out_duration = 0;

    // Independent transition at this clip's IN (leading) boundary. `transition_in`
    // fades the clip in from black (or the underlying track) over the first
    // `transition_in_duration` frames, starting at tl_in. This lets a clip fade in
    // at its head with no preceding clip/cut required, independent of
    // `transition_out`. Shares the same TransitionType vocabulary as OUT (FadeIn /
    // CrossDissolve from black, etc.).
    TransitionType transition_in = TransitionType::None;
    int64_t transition_in_duration = 0;

    [[nodiscard]] bool is_linked() const noexcept { return linked_id != 0; }
    [[nodiscard]] int64_t duration() const { return tl_out - tl_in; }
    [[nodiscard]] bool has_transition_out() const noexcept {
        return transition_out != TransitionType::None && transition_out_duration > 0;
    }
    [[nodiscard]] bool has_transition_in() const noexcept {
        return transition_in != TransitionType::None && transition_in_duration > 0;
    }
    [[nodiscard]] bool has_transition() const noexcept {
        return has_transition_out() || has_transition_in();
    }
};

struct Track {
    enum class Kind { Video, Audio };

    Kind kind = Kind::Video;
    std::string name;
    bool locked = false;
    std::vector<Clip> clips;

    [[nodiscard]] const Clip* clip_at(int64_t pos) const noexcept;
    [[nodiscard]] const Clip* clip_with_id(ClipId id) const noexcept;
    void insert_sorted(Clip clip);
    [[nodiscard]] int64_t end_frame() const noexcept;
};

struct Bookmark {
    int64_t frame = 0;
    std::string label;
    uint64_t id = 0;
};

struct Sequence {
    double fps = 30.0;
    std::vector<Track> video_tracks;
    std::vector<Track> audio_tracks;
    std::vector<Bookmark> bookmarks;

    [[nodiscard]] int64_t duration_frames() const noexcept;
    [[nodiscard]] Track* track(Track::Kind kind, std::size_t index) noexcept;
    [[nodiscard]] const Track* track(Track::Kind kind, std::size_t index) const noexcept;
    [[nodiscard]] std::size_t track_count(Track::Kind kind) const noexcept;

    [[nodiscard]] bool has_bookmark(int64_t frame) const noexcept;
    [[nodiscard]] uint64_t toggle_bookmark(int64_t frame, const std::string& label = "");
    bool remove_bookmark(uint64_t id);
    void remove_bookmark_at(int64_t frame);

    ClipId next_clip_id = 1;
    uint64_t next_bookmark_id = 1;
};

}
