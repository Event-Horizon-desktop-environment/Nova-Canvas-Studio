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
    std::string name{};
    ClipId linked_id = 0;
    bool enabled = true;

    TransitionType transition_out = TransitionType::None;
    int64_t transition_out_duration = 0;

    TransitionType transition_in = TransitionType::None;
    int64_t transition_in_duration = 0;

    float transition_out_curve_value = 1.0f;
    float transition_out_ease = 0.0f;
    float transition_in_curve_value = 0.0f;
    float transition_in_ease = 0.0f;
    int transition_out_start_ratio = 0;
    int transition_out_end_ratio = 100;
    int transition_in_start_ratio = 0;
    int transition_in_end_ratio = 100;

    float volume_db = 0.0f;
    float pan = 0.0f;

    float scale_x = 1.0f;
    float scale_y = 1.0f;
    double pos_x = 0.0;
    double pos_y = 0.0;
    float rotation_deg = 0.0f;
    double anchor_dx = 0.0;
    double anchor_dy = 0.0;
    bool flip_h = false;
    bool flip_v = false;

    float opacity = 1.0f;
    BlendMode blend_mode = BlendMode::Normal;

    grade_graph::GradeGraph grade{};

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
        bool enabled = true;
        bool operator==(const EqBand&) const = default;
    };
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

    VoiceIsolationMode voice_isolation = VoiceIsolationMode::None;

    enum class ClipTag : uint8_t { None = 0, GoodTake, Rejected };
    ClipTag clip_tag = ClipTag::None;
    uint8_t clip_color = 0;
    std::string comments{};

    struct Title {
        std::string text;
        float size = 0.1f;
        float r = 1.0f;
        float g = 1.0f;
        float b = 1.0f;
        float a = 1.0f;
        std::string font_family;
        bool bold = false;
        bool italic = false;
        bool underline = false;
        bool shadow = false;
        float shadow_dx = 2.0f;
        float shadow_dy = 2.0f;
        float shadow_blur = 2.0f;
        float shadow_opacity = 0.6f;
        float shadow_r = 0.0f;
        float shadow_g = 0.0f;
        float shadow_b = 0.0f;
        bool box = false;
        float box_pad_x = 12.0f;
        float box_pad_y = 8.0f;
        float box_radius = 0.0f;
        float box_opacity = 0.6f;
        float box_r = 0.0f;
        float box_g = 0.0f;
        float box_b = 0.0f;
        [[nodiscard]] bool is_title() const noexcept { return !text.empty(); }
        bool operator==(const Title&) const = default;
    };
    Title title{};

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
    [[nodiscard]] bool has_visual_transform() const noexcept {
        return scale_x != 1.0f || scale_y != 1.0f || pos_x != 0.0 || pos_y != 0.0 ||
               rotation_deg != 0.0f || anchor_dx != 0.0 || anchor_dy != 0.0 ||
               flip_h || flip_v;
    }
    [[nodiscard]] bool has_grade() const noexcept { return !grade.edges().empty(); }
    [[nodiscard]] bool has_title() const noexcept { return title.is_title(); }
    [[nodiscard]] bool needs_compositing() const noexcept {
        return opacity != 1.0f || blend_mode != BlendMode::Normal;
    }
};

struct Track {
    enum class Kind { Video, Audio };

    Kind kind = Kind::Video;
    std::string name;
    bool locked = false;
    bool collapsed = false;
    bool muted = false;
    bool solo = false;
    float gain_db = 0.0f;
    std::vector<Clip> clips;

    [[nodiscard]] const Clip* clip_at(int64_t pos) const noexcept;
    [[nodiscard]] const Clip* clip_with_id(ClipId id) const noexcept;
    void insert_sorted(Clip clip);
    [[nodiscard]] int64_t end_frame() const noexcept;
};

struct Bookmark {
    int64_t frame = 0;
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
    [[nodiscard]] uint64_t add_range(int64_t in, int64_t out, const std::string& label = "");
    [[nodiscard]] std::vector<Bookmark> bookmarks_in(int64_t in, int64_t out) const;
    bool remove_bookmark(uint64_t id);
    void remove_bookmark_at(int64_t frame);

    ClipId next_clip_id = 1;
    uint64_t next_bookmark_id = 1;
};

}
