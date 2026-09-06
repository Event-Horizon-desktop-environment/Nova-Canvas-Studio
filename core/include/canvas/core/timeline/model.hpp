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

// How a video clip composites over the content beneath it (lower tracks then
// the black background). `Normal` is a plain alpha-over.
enum class BlendMode {
    Normal = 0,
    Add,
    Multiply,
    Screen,
    Overlay,
};

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

    // Audio mix parameters. `volume_db` is the gain in decibels (0 = unity);
    // `pan` ranges -1.0 (hard left) .. +1.0 (hard right), 0 = center. Applied to
    // this clip's audio (embedded or on an audio track) during playback and
    // export. Stored per-clip so the existing track-snapshot undo machinery
    // restores them automatically.
    float volume_db = 0.0f;
    float pan = 0.0f;

    // Visual transform (video clips). `scale_x`/"scale_y" are the Zoom factor
    // (1 = 100%); `pos_x`/"pos_y" shift the content in output pixels (0 =
    // centered); `rotation_deg` is the rotation about the anchor; the anchor is
    // an offset in pixels from the fitted frame's CENTER (0 = center, matching
    // the reference inspector's "Anchor Point"); `flip_h`/"flip_v" mirror the
    // source around its center. All identity by default, so existing projects
    // (and untouched clips) composite exactly as before. Audio clips carry the
    // same fields (an A/V pair shares them) but they only affect the video half.
    float scale_x = 1.0f;
    float scale_y = 1.0f;
    double pos_x = 0.0;
    double pos_y = 0.0;
    float rotation_deg = 0.0f;
    double anchor_dx = 0.0;
    double anchor_dy = 0.0;
    bool flip_h = false;
    bool flip_v = false;

    // Composite (video clips): `opacity` 0..1 (1 = opaque) and the blend mode
    // used when layering this clip over lower tracks / the background.
    float opacity = 1.0f;
    BlendMode blend_mode = BlendMode::Normal;

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
    // True when any visual transform field deviates from its identity default
    // (the fast-path compositing check).
    [[nodiscard]] bool has_visual_transform() const noexcept {
        return scale_x != 1.0f || scale_y != 1.0f || pos_x != 0.0 || pos_y != 0.0 ||
               rotation_deg != 0.0f || anchor_dx != 0.0 || anchor_dy != 0.0 ||
               flip_h || flip_v;
    }
    // True when the clip draws non-opaque (transparency / blend mode).
    [[nodiscard]] bool needs_compositing() const noexcept {
        return opacity != 1.0f || blend_mode != BlendMode::Normal;
    }
};

struct Track {
    enum class Kind { Video, Audio };

    Kind kind = Kind::Video;
    std::string name;
    bool locked = false;
    // Audio mixing state. `muted` silences the track entirely; `solo` isolates
    // it: if ANY audio track is soloed, only soloed tracks are audible (mute
    // still beats solo). Meaningless for video tracks, which carry no mix.
    bool muted = false;
    bool solo = false;
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
