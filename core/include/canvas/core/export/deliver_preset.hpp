#pragma once

#include "canvas/core/export/exporter.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace canvas::core {

enum class RenderScope {
    SingleClip,
    IndividualClips,
    Still,
    FrameSequence,
};

enum class EncoderBackend {
    Auto,
    CPU,
    NVIDIA,
    AMD,
    Intel,
};

enum class VideoCodec {
    H264,
    H265,
    AV1,
    ProRes,
    FFV1,
    JPEG2000,
    Uncompressed,
};

enum class EncodingProfile {
    Main,
    Main10,
    Main422,
    Main42210,
    Main444,
    Main44410,
};

enum class RateControl {
    ConstantQP,
    VBRQuality,
    VBRTargetKbps,
    ConstantBitrate,
};

enum class MultiEncode {
    Auto,
    Enabled,
    Disabled,
};

enum class EncoderTuning {
    HighQuality,
    LowLatency,
    UltraLowLatency,
    Lossless,
};

enum class PixelAspect {
    Square,
    Cinemascope,
};

enum class DataLevels {
    Auto,
    Video,
    Full,
};

enum class KeyFrameMode {
    Automatic,
    EveryNFrames,
};

std::string video_encoder_name(VideoCodec codec, EncoderBackend backend,
                               const std::string& container_format, bool* sw_fallback);

std::string container_format_name(const std::string& format);

[[nodiscard]] bool scope_is_still(RenderScope scope) noexcept;
[[nodiscard]] bool scope_is_sequence(RenderScope scope) noexcept;
[[nodiscard]] bool scope_is_image(RenderScope scope) noexcept;

[[nodiscard]] std::string still_output_path(const std::string& base);
[[nodiscard]] std::string sequence_output_path(const std::string& base, int64_t frame_index);

VideoCodec video_codec_from_string(const std::string& codec);

ExportSettings to_export_settings(const struct DeliverSettings& ds);

struct DeliverVideoSettings {
    bool export_video = true;
    std::string format = "MKV";
    std::string codec = "H.265";
    EncoderBackend encoder = EncoderBackend::Auto;
    bool network_optimization = false;
    std::string resolution = "Timeline Resolution";
    int custom_width = 1920;
    int custom_height = 1080;
    bool use_vertical_resolution = false;
    std::string frame_rate = "Timeline Frame Rate";
    double custom_fps = 60.0;
    bool export_alpha = false;
    bool chapters_from_markers = false;
    EncodingProfile encoding_profile = EncodingProfile::Main;
    KeyFrameMode key_frames = KeyFrameMode::Automatic;
    int key_frame_interval = 30;
    bool frame_reordering = true;
    RateControl rate_control = RateControl::ConstantBitrate;
    int quality = 0;
    int target_bitrate_kbps = 80000;
    int max_bitrate_kbps = 80000;
    MultiEncode multi_encode = MultiEncode::Enabled;
    std::string preset = "Medium";
    EncoderTuning tuning = EncoderTuning::HighQuality;
    bool two_pass = false;
    int lookahead_frames = 16;
    int lookahead_level = 0;
    bool adaptive_i_at_scene_cuts = false;
    bool adaptive_b_frame = true;
    int aq_strength = 8;
    bool non_reference_p_frame = false;
    bool weighted_prediction = false;
    bool temporal_filtering = false;
    bool unidirectional_b_frames = false;

    [[nodiscard]] bool enable_b_frames() const noexcept { return adaptive_b_frame; }

    PixelAspect pixel_aspect = PixelAspect::Square;
    DataLevels data_levels = DataLevels::Auto;
    bool retain_sub_black_super_white = false;
    std::string color_space_tag = "Same as project";
    std::string gamma_tag = "Same as project";
    std::string data_burn_in = "Same as project";
    bool bypass_reenecode_when_possible = true;
    bool render_all_video_tracks = true;
    bool force_sizing_high_quality = false;
    bool force_debayer_high_quality = false;
    std::string flat_pass = "Off";
    std::string visionos_bypass = "Off";
    bool disable_sizing_and_blanking = false;
};

struct DeliverAudioSettings {
    bool export_audio = true;
    std::string codec = "AAC";
    int bitrate_kbps = 192;
    int sample_rate = 48000;
    int channels = 2;
    bool render_track_audio = true;
    bool normalize_audio = false;
    float normalize_target_lufs = -23.0f;
};

struct DeliverFileSettings {
    std::string file_name = "Untitled";
    std::string location;
    bool embed_media = false;
};

struct DeliverAdvancedSettings {
    int threads = 0;
    bool enable_pipewire = false;
    bool disallow_masking_metadata = false;
    std::string extra_options;
};

struct DeliverSettings {
    std::string preset_name = "Custom Export";
    RenderScope render_scope = RenderScope::SingleClip;
    DeliverVideoSettings video;
    DeliverAudioSettings audio;
    DeliverFileSettings file;
    DeliverAdvancedSettings advanced;
};

struct RenderJobSnapshot {
    uint64_t id = 0;
    std::string name;
    std::string output_path;
    DeliverSettings settings;
    int64_t total_frames = 0;
    int priority = 0;
    int status = 0;
    double progress = 0.0;
    double render_fps = 0.0;
    std::string error;
    double elapsed_seconds = 0.0;
    int64_t frames_rendered = 0;
    std::string finished_at;
};

std::vector<std::string> deliver_formats();
std::vector<std::string> deliver_video_codecs();
std::vector<std::string> deliver_audio_codecs();
std::vector<std::string> deliver_encoders();

}