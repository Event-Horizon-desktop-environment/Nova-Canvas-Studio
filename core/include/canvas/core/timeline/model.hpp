#pragma once

#include "canvas/core/grade_graph/graph.hpp"
#include "canvas/core/media/voice_isolation.hpp"

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
// the black background). `Normal` is a plain alpha-over. The first five values
// are the original set and MUST keep their numeric values (project files store
// the int); Phase 1 (F6) appended SoftLight/Subtract/Difference. The byte law
// for these lives in timeline/blend.hpp, defined through the color-page
// grade_graph::BlendMode math so the two can never drift.
enum class BlendMode {
    Normal = 0,
    Add,
    Multiply,
    Screen,
    Overlay,
    SoftLight,
    Subtract,
    Difference,
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
        // Per-band bypass: a disabled band is excluded from the cascade and the
        // displayed curve, but keeps its settings so toggling it back on restores
        // the exact band. Defaults to enabled so existing clips/bands behave
        // identically.
        bool enabled = true;
        bool operator==(const EqBand&) const = default;
    };
    // The EQ curve a clip starts with. Flat by default: enabling EQ must be
    // silent until the user shapes it (a 0 dB five-band lift would otherwise
    // boom the mix the moment EQ is toggled on). All-Bell so a fresh EQ is a
    // bit-exact pass-through (LowPass/HighPass/Notch always filter, even at
    // 0 dB of gain).
    [[nodiscard]] static std::array<EqBand, 6> default_eq_bands() noexcept {
        std::array<EqBand, 6> bands{};
        for (auto& b : bands) {
            b.type = EqBand::Type::Bell;
            b.frequency = 1000.0f;
            b.gain = 0.0f;
            b.q = 1.0f;
        }
        return bands;
    }
    bool eq_enabled = false;
    std::array<EqBand, 6> eq_bands = default_eq_bands();

    // AI voice isolation engine applied to this clip's audio BEFORE its gains
    // and mix, in both playback and export. `None` by default so existing clips
    // (and untouched projects) decode identically. Stored per-clip so the
    // track-snapshot undo machinery restores it automatically. The engine names
    // in `voice_isolation_mode_name()` back the Inspector's dropdown.
    VoiceIsolationMode voice_isolation = VoiceIsolationMode::None;

    // Clip metadata — tag (good-take / rejected), colour swatch (0 = none,
    // 1–12 = swatch index), and free-form comments. Stored per-clip so undo
    // restores them.
    enum class ClipTag : uint8_t { None = 0, GoodTake, Rejected };
    ClipTag clip_tag = ClipTag::None;
    uint8_t clip_color = 0;
    std::string comments;

    // Title overlay / generator text. When `text` is non-empty the clip draws a
    // title: `media` stays -1 for a pure title clip (its video is the rasterised
    // text over the top media clip beneath it, or over black), and a media clip
    // may also carry a title burned over its own footage. `size` is the glyph em
    // height as a fraction of the output frame height (clamped to the title law
    // bounds); `r`/`g`/`b`/`a` are the text colour, all 0..1. Stored per-clip so
    // the track-snapshot undo machinery and the project round-trip restore it
    // automatically.
    struct Title {
        std::string text;  // UTF-8; empty = no title overlay
        float size = 0.1f;
        float r = 1.0f;
        float g = 1.0f;
        float b = 1.0f;
        float a = 1.0f;
        // Typeface family (e.g. "DejaVu Sans"); empty = the default system
        // font. Resolved at render time via title::find_font_path_for, so an
        // unknown family degrades to the default face rather than to nothing.
        std::string font_family;
        // Faux styles synthesised by the rasteriser (embolden overdraw, slant,
        // baseline bar) — independent of the installed faces, so they work
        // with any family including the default face.
        bool bold = false;
        bool italic = false;
        bool underline = false;
        // Drop shadow behind the glyphs: offset (px, right/down positive),
        // blur radius (px, 0 = hard), opacity, colour. Disabled by default.
        bool shadow = false;
        float shadow_dx = 2.0f;
        float shadow_dy = 2.0f;
        float shadow_blur = 2.0f;
        float shadow_opacity = 0.6f;
        float shadow_r = 0.0f;
        float shadow_g = 0.0f;
        float shadow_b = 0.0f;
        // Background box behind the whole title block: padding (px), corner
        // radius (px, 0 = square), opacity, colour. Disabled by default.
        bool box = false;
        float box_pad_x = 12.0f;
        float box_pad_y = 8.0f;
        float box_radius = 0.0f;
        float box_opacity = 0.6f;
        float box_r = 0.0f;
        float box_g = 0.0f;
        float box_b = 0.0f;
        [[nodiscard]] bool is_title() const noexcept { return !text.empty(); }
        // Exact field-wise equality for the undo no-op detection (floats
        // compare exactly — snapshot semantics, not visual tolerance).
        bool operator==(const Title&) const = default;
    };
    Title title;

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
    // True when this clip draws a title overlay (burned over its own media or,
    // for a media < 0 title clip, over the content beneath). Drives the GPU
    // fast-path bail and the Inspector's Title category.
    [[nodiscard]] bool has_title() const noexcept { return title.is_title(); }
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
    // UI-only display flag: the header shows a collapsed chevron and the body
    // renders at kCollapsedTrackHeight in the timeline. Stored on the model so
    // it survives project round-trips and undo (see set_track_collapsed), even
    // though it never touches playback/export math.
    bool collapsed = false;
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
    // Range end. 0 (the default) means a point marker at `frame`; a value
    // `> frame` means a named RANGE [frame, tl_out). `frame` stays the marker
    // START so every existing point-marker lookup (has_bookmark, toggle) is
    // unchanged.
    int64_t tl_out = 0;
    std::string label;
    uint64_t id = 0;

    [[nodiscard]] bool is_range() const noexcept { return tl_out > frame; }
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
    // Add a named RANGE [in, out). `out <= in` degenerates to a point marker.
    // Returns the new bookmark id. Keeps `bookmarks` sorted by start frame.
    [[nodiscard]] uint64_t add_range(int64_t in, int64_t out, const std::string& label = "");
    // Bookmarks whose START frame lies in [in, out) — the export-from-range
    // query. Ranges are included when their start is inside (a range that merely
    // overlaps the window but starts before it is not captured).
    [[nodiscard]] std::vector<Bookmark> bookmarks_in(int64_t in, int64_t out) const;
    bool remove_bookmark(uint64_t id);
    void remove_bookmark_at(int64_t frame);

    ClipId next_clip_id = 1;
    uint64_t next_bookmark_id = 1;
};

}
