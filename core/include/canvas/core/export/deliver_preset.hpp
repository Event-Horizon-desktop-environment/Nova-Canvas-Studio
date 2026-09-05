#pragma once

#include "canvas/core/export/exporter.hpp"

#include <string>
#include <vector>

namespace canvas::core {

// ---------------------------------------------------------------------------
// Deliver page settings — a high-level model that maps onto the
// lower-level FFmpeg ExportSettings used by export_project(). It captures every
// option shown in the Deliver settings panel (Video/Audio/File tabs, plus the
// advanced render options) so the GUI can build controls over it and translate
// it deterministically into ExportSettings.
// ---------------------------------------------------------------------------

// Render scope for a job.
enum class RenderScope {
    SingleClip,      // render the whole timeline as one clip
    IndividualClips, // one output per clip (queue fills with one job per clip)
};

// Encoder backend selection.
enum class EncoderBackend {
    Auto,    // pick GPU if available for the codec, else CPU
    CPU,     // force software encoder (libx264/libx265/libsvtav1/...)
    NVIDIA,  // NVENC
    AMD,     // VAAPI
    Intel,   // QSV
};

// Video codec family (FFmpeg encoder is derived from codec + backend + format).
enum class VideoCodec {
    H264,
    H265,
    AV1,
    ProRes,
    FFV1,
    JPEG2000,
    Uncompressed,
};

// Video encoding profile (H.26x / ProRes flavor).
enum class EncodingProfile {
    Main,
    Main10,
    Main422,
    Main42210,
    Main444,
    Main44410,
};

// Rate-control mode.
enum class RateControl {
    ConstantQP,
    VBRQuality,      // Variable Bitrate (Quality)
    VBRTargetKbps,   // Variable Bitrate (Target Kbps)
    ConstantBitrate,
};

// Multi-encode (parallel chunked encode).
enum class MultiEncode {
    Auto,
    Enabled,
    Disabled,
};

// Encoder tuning.
enum class EncoderTuning {
    HighQuality,
    LowLatency,
    UltraLowLatency,
    Lossless,
};

// Pixel aspect ratio.
enum class PixelAspect {
    Square,
    Cinemascope,
};

// Data levels.
enum class DataLevels {
    Auto,
    Video,
    Full,
};

// Frame key-frame placement.
enum class KeyFrameMode {
    Automatic,
    EveryNFrames,
};

// Converts a VideoCodec + EncoderBackend (+ requested container) into a concrete
// FFmpeg encoder name. Falls back to a software encoder when hardware isn't
// supported ("sw_fallback" set) or when backend == CPU.
std::string video_encoder_name(VideoCodec codec, EncoderBackend backend,
                               const std::string& container_format, bool* sw_fallback);

// Maps a friendly container/format name (MKV, MP4, QuickTime, ...) to the FFmpeg
// muxer name. Empty string when unknown.
std::string container_format_name(const std::string& format);

// Maps a friendly codec display name to VideoCodec.
VideoCodec video_codec_from_string(const std::string& codec);

// Translates a fully-specified DeliverSettings into the low-level ExportSettings
// consumed by export_project().
ExportSettings to_export_settings(const struct DeliverSettings& ds);

// All video render options for one export job (the Deliver left panel).
struct DeliverVideoSettings {
    bool export_video = true;
    std::string format = "MKV";                 // container display name
    std::string codec = "H.265";                // display name
    EncoderBackend encoder = EncoderBackend::Auto;
    bool network_optimization = false;          // frag -> mp4/mov
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
    int quality = 0;                           // CRF / quality value (lower = better, 0 = best)
    int target_bitrate_kbps = 80000;
    int max_bitrate_kbps = 80000;
    MultiEncode multi_encode = MultiEncode::Enabled;
    std::string preset = "Faster";
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

    // True when adaptive B-frames are enabled (used to emit bf=).
    [[nodiscard]] bool enable_b_frames() const noexcept { return adaptive_b_frame; }

    // Advanced
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
    bool export_audio = true;                  // audio exported by default (matches "remove_audio")
    std::string codec = "AAC";                 // display name ("" = no audio)
    int bitrate_kbps = 192;
    int sample_rate = 48000;
    int channels = 2;
    bool render_track_audio = true;
    bool normalize_audio = false;
    float normalize_target_lufs = -23.0f;
};

struct DeliverFileSettings {
    std::string file_name = "Untitled";
    std::string location;                      // empty = ask at render time
    bool embed_media = false;
};

// Advanced/other (kept for completeness; maps to extra libavcodec opts).
struct DeliverAdvancedSettings {
    int threads = 0;
    bool enable_pipewire = false;
    bool disallow_masking_metadata = false;
    std::string extra_options;                 // raw "k=v\nk=v" passthrough
};

// One complete set of deliver settings (what the left panel edits).
struct DeliverSettings {
    std::string preset_name = "Custom Export";
    RenderScope render_scope = RenderScope::SingleClip;
    DeliverVideoSettings video;
    DeliverAudioSettings audio;
    DeliverFileSettings file;
    DeliverAdvancedSettings advanced;
};

// Nice human labels for enumerations (used by the GUI to populate combo boxes).
std::vector<std::string> deliver_formats();     // MKV, MP4, QuickTime, ...
std::vector<std::string> deliver_video_codecs(); // H.264, H.265, AV1, ProRes, ...
std::vector<std::string> deliver_audio_codecs(); // AAC, MP3, PCM, ...
std::vector<std::string> deliver_encoders();     // Auto, CPU, NVIDIA, AMD, Intel

}  // namespace canvas::core
