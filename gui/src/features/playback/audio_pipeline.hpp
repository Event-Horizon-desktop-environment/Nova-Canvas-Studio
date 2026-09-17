#pragma once

#include "canvas/core/media/audio_decoder.hpp"
#include "canvas/core/media/equalizer.hpp"
#include "canvas/core/media/voice_isolation.hpp"
#include "canvas/core/project/project.hpp"
#include "canvas/core/timeline/time_stretch.hpp"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

namespace canvas::gui {

class AudioSink;

class AudioPipeline {
public:
    explicit AudioPipeline(AudioSink& sink) : sink_(sink) {}
    ~AudioPipeline();
    AudioPipeline(const AudioPipeline&) = delete;
    AudioPipeline& operator=(const AudioPipeline&) = delete;

    void set_project(const canvas::core::Project* project);
    void update_project(const canvas::core::Project* project);
    void add_media(const canvas::core::MediaEntry& entry);
    void reset();

    [[nodiscard]] bool is_active() const { return active_; }
    [[nodiscard]] bool is_open() const;
    [[nodiscard]] AudioSink& sink() { return sink_; }

    void open_output();
    void close_output();
    void set_channels(int channels) {
        if (!active_) channels_ = channels > 0 ? channels : 2;
    }
    [[nodiscard]] int channels() const { return channels_; }

    void set_live_clip_gain(canvas::core::ClipId id, float volume_db);
    void clear_live_clip_gain(canvas::core::ClipId id);
    void clear_live_clip_gains();

    void rewind(int64_t seq_frame, bool playing);

    void preroll(int64_t seq_frame, int lead_ms, bool playing);

    void play_step(int64_t seq_frame, double step_seconds, bool seek_hold_active);

    void play_scrub_grain(int64_t seq_frame);

    void feed_scrub_audio(int64_t target);

    void begin_scrub();
    [[nodiscard]] int repositions_since_begin() const { return scrub_repositions_; }

    int64_t audible_seq_frame(int64_t playhead_seq) const;
    [[nodiscard]] bool feed_watermark_active() const { return !feed_watermarks_.empty(); }
    void advance_feed_for_drop(int64_t new_frame);

    canvas::core::MediaId audio_media_at(int64_t seq_frame) const;

    void set_wave_capture(const char* path);
    void close_wave_capture();

private:
    struct AudioSource {
        const canvas::core::Clip* clip = nullptr;
        double fps = 0.0;
        float gain_db = 0.0f;
    };

    const canvas::core::Clip* audio_clip_at(int64_t seq_frame) const;
    std::vector<AudioSource> audible_sources_at(int64_t seq_frame) const;
    int64_t write_mixed(int64_t seq_frame, int64_t want_frames);

    const canvas::core::Project* project_ = nullptr;
    const canvas::core::Clip* clip_at_any_track(int64_t seq_frame) const;
    float effective_clip_volume_db(const canvas::core::Clip& clip) const;
    int64_t playhead_to_audio_sample(int64_t seq_frame) const;
    int64_t audio_sample_to_seq_frame(int64_t media_sample) const;
    void log_av_sync(int64_t seq_frame, double video_fps, double step_seconds);
    void reanchor_locked(int64_t seq_frame);

    AudioSink& sink_;

    std::unordered_map<canvas::core::MediaId, std::unique_ptr<canvas::core::AudioDecoder>>
        decoders_;
    mutable std::mutex mutex_;

    int rate_ = 48000;
    int channels_ = 2;
    bool active_ = false;

    std::unordered_map<canvas::core::ClipId, float> live_gain_db_;

    canvas::core::VoiceIsolationBank iso_bank_;

    canvas::core::EqualizerBank eq_bank_;

    canvas::core::TimeStretchBank stretch_bank_;

    int64_t anchor_media_sample_ = 0;
    int64_t anchor_seq_frame_ = 0;
    uint64_t written_at_anchor_ = 0;
    uint64_t run_id_ = 0;
    uint64_t speed_run_ = 0;
    double speed_video_ms_ = 0.0;
    double speed_audible_ms_ = 0.0;

    std::unordered_map<canvas::core::ClipId, int64_t> feed_watermarks_;

    int64_t last_seq_fed_ = -1;

    std::chrono::steady_clock::time_point last_drift_anchor_{};

    int64_t last_scrub_audio_target_ = -1;
    std::chrono::steady_clock::time_point last_scrub_audio_at_{};
    int scrub_repositions_ = 0;

    std::FILE* wav_capture_f_ = nullptr;
    std::string wav_capture_path_;
    uint64_t wav_capture_frames_ = 0;
    bool wav_capture_disabled_ = false;
    uint64_t feed_ledger_frames_ = 0;
    std::chrono::steady_clock::time_point feed_ledger_at_{};
    canvas::core::ClipId last_primary_clip_ = 0;
    int cut_diag_ = 0;
    double step_decode_ms_ = 0.0;
    double step_mix_ms_ = 0.0;
    double step_write_ms_ = 0.0;
    void maybe_wave_capture_open_locked();
    void wave_capture_write_locked(const float* data, std::size_t frames);
    void wave_capture_close_locked();
    void log_feed_ledger_locked(int64_t seq_frame, int64_t start_sample, int64_t from,
                                int64_t want, int64_t written);
};

}
