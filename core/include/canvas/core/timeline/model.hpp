#pragma once

#include "canvas/core/grade_graph/graph.hpp"

#include <array>
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

    // Transition shaping, stored PER EDGE so the inspector's Start (OUT) and
    // End (IN) sub-tabs are genuinely independent. `curve_value` is the curve
    // position 0..1 (0 = fully the outgoing frame's style, 1 = the incoming);
    // `ease` 0..1 drives the ease amount (0 = linear). `start_ratio`/`end_ratio`
    // (0..100) carve the fade profile out of the transition window: 0 = the
    // bubble's left edge, 100 = its right edge, so the default 0/100 spans the
    // whole window symmetrically. Defaults match the reference inspector: the
    // OUT (Start) curve resolves fully incoming (1.0), the IN (End) curve stays
    // outgoing (0.0).
    float transition_out_curve_value = 1.0f;
    float transition_out_ease = 0.0f;
    float transition_in_curve_value = 0.0f;
    float transition_in_ease = 0.0f;
    int transition_out_start_ratio = 0;
    int transition_out_end_ratio = 100;
    int transition_in_start_ratio = 0;
    int transition_in_end_ratio = 100;

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

    // Color grade: the node-grade tree applied to this clip's video BEFORE the
    // transform/composite blit (Phase 3). An empty graph (no wired nodes) is
    // the reporter's "no grade" — the evaluator passes the frame through and
    // the project file omits the `grade` key, so ungraded clips save
    // byte-identically to legacy files. Stored per-clip so the track-snapshot
    // undo machinery restores it automatically. Only meaningful on video clips;
    // an A/V pair's audio half never grades.
    grade_graph::GradeGraph grade;

    // Audio processing — pitch shift, speed change, and parametric EQ. These
    // fields are stored per-clip so track-snapshot undo restores them
    // automatically. Pitch is split into semitones (coarse) and cents (fine);
    // speed is a linear factor (1.0 = unity); the EQ has 6 bands with
    // configurable type/frequency/gain/Q.
    float pitch_semitones = 0.0f;
    float pitch_cents = 0.0f;
    float speed_factor = 1.0f;
    bool speed_enabled = false;

    struct EqBand {
        enum class Type : uint8_t { LowShelf = 0, Bell, HighShelf, LowPass, HighPass, Notch };
        Type type = Type::Bell;
        float frequency = 1000.0f;
        float gain = 0.0f;
        float q = 1.0f;
        bool operator==(const EqBand&) const = default;
    };
    // The reference EQ curve shown when a clip first gains EqBand defaults
    // (mirrors the reference app's six-band layout).
    [[nodiscard]] static std::array<EqBand, 6> default_eq_bands() noexcept {
        std::array<EqBand, 6> bands{};
        bands[0].type = EqBand::Type::LowShelf;
        bands[0].frequency = 20.0f;
        bands[0].gain = 18.1f;
        bands[1].type = EqBand::Type::Bell;
        bands[1].frequency = 57.0f;
        bands[1].gain = 18.1f;
        bands[2].type = EqBand::Type::Bell;
        bands[2].frequency = 97.0f;
        bands[2].gain = 10.5f;
        bands[2].q = 1.0f;
        bands[3].type = EqBand::Type::Bell;
        bands[3].frequency = 1200.0f;
        bands[3].gain = 0.0f;
        bands[3].q = 1.0f;
        bands[4].type = EqBand::Type::HighShelf;
        bands[4].frequency = 6000.0f;
        bands[4].gain = 0.0f;
        bands[5].type = EqBand::Type::LowPass;
        bands[5].frequency = 19000.0f;
        bands[5].gain = 0.0f;
        return bands;
    }
    bool eq_enabled = false;
    std::array<EqBand, 6> eq_bands = default_eq_bands();

    // Clip metadata — tag (good-take / rejected), colour swatch (0 = none,
    // 1–12 = swatch index), and free-form comments. Stored per-clip so undo
    // restores them.
    enum class ClipTag : uint8_t { None = 0, GoodTake, Rejected };
    ClipTag clip_tag = ClipTag::None;
    uint8_t clip_color = 0;
    std::string comments;

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
    // True when the clip carries a wired grade tree that the renderer must
    // apply. Nodes alone (no wires) are a no-op tree and read as "no grade",
    // so they keep the fast path.
    [[nodiscard]] bool has_grade() const noexcept { return !grade.edges().empty(); }
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
    // Post-fade, pre-sum gain applied to every audible clip on this audio track
    // during playback and export (dB, shared audio_mix law). Meaningless for
    // video tracks, which carry no audio mix.
    float gain_db = 0.0f;
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
