#pragma once

#include <QObject>
#include <QString>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>

#include "canvas/core/media/frame.hpp"
#include "canvas/core/project/project.hpp"
#include "canvas/core/timeline/model.hpp"

#include "audio_output.hpp"
#include "audio_pipeline.hpp"
#include "sonicsync.hpp"
#include "sync_constants.hpp"
#include "timeline_decoder.hpp"

namespace canvas::gui {

class SequenceController final : public QObject {
    Q_OBJECT

public:
    explicit SequenceController(QObject* parent = nullptr);
    ~SequenceController() override;

    void set_project(std::shared_ptr<const canvas::core::Project> project, int64_t initial_frame = -1);
    void swap_project(std::shared_ptr<const canvas::core::Project> project);
    void update_audio_mix(std::shared_ptr<const canvas::core::Project> project);
    void set_live_clip_gain(canvas::core::ClipId id, float volume_db) { audio_.set_live_clip_gain(id, volume_db); }
    void clear_live_clip_gain(canvas::core::ClipId id) { audio_.clear_live_clip_gain(id); }
    void clear_live_clip_gains() { audio_.clear_live_clip_gains(); }
    void add_media(const canvas::core::MediaEntry& entry);
    void play();
    void pause();
    void toggle_play_pause();
    void release_audio();
    void seek(int64_t frame_number);
    void seek_preview(int64_t frame_number);
    void begin_scrub();
    void end_scrub();
    void step(int64_t delta);
    void set_scrub_audio_enabled(bool on) { scrub_audio_enabled_.store(on); }
    [[nodiscard]] bool scrub_audio_enabled() const { return scrub_audio_enabled_.load(); }

    void set_volume(float volume) { audio_out_.set_volume(volume); }
    [[nodiscard]] float volume() const { return audio_out_.volume(); }
    void set_muted(bool muted) { audio_out_.set_muted(muted); }
    [[nodiscard]] bool muted() const { return audio_out_.muted(); }
    void set_dimmed(bool dimmed) { audio_out_.set_dimmed(dimmed); }
    [[nodiscard]] bool dimmed() const { return audio_out_.dimmed(); }

    [[nodiscard]] bool is_playing() const { return playing_.load(); }
    [[nodiscard]] int64_t current_frame() const { return current_frame_.load(); }
    [[nodiscard]] double fps() const;
    [[nodiscard]] int64_t total_frames() const { return total_frames_.load(); }

signals:
    void frame_ready(canvas::core::RenderFramePtr frame);
    void position_changed(int64_t frame_number);
    void playback_changed(bool playing);

private:
    enum class Command { SetProject, SwapProject, AddMedia, Play, Pause, Seek, SeekPreview, Step, UpdateAudioMix, ReleaseAudio, Stop };
    struct Request {
        Command command = Command::Stop;
        int64_t arg = 0;
        std::shared_ptr<const canvas::core::Project> project;
        canvas::core::MediaEntry media;
    };

    void worker_loop();
    void push(Request request);
    void handle_set_project(std::shared_ptr<const canvas::core::Project> project, int64_t initial_frame);
    void handle_swap_project(std::shared_ptr<const canvas::core::Project> project);
    void handle_update_audio_mix(std::shared_ptr<const canvas::core::Project> project);
    void handle_add_media(const canvas::core::MediaEntry& entry);
    void handle_play();
    void handle_seek(int64_t frame_number);
    void handle_seek_preview(int64_t frame_number);
    void warm_lookahead(int64_t start_frame);
    void reset_ready();
    void fill_lookahead(int64_t base);
    void present_next();
    canvas::core::RenderFramePtr frame_for_playhead(int64_t seq_frame);
    canvas::core::RenderFramePtr frame_for_playhead_preview(int64_t seq_frame, int max_dim);

    std::shared_ptr<const canvas::core::Project> project_;

    TimelineDecoder decoder_;

    std::thread worker_;
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<Request> queue_;

    std::deque<canvas::core::RenderFramePtr> ready_;
    int64_t ready_base_ = -1;
    std::atomic<bool> playing_{false};
    std::atomic<bool> stopping_{false};
    std::atomic<bool> scrubbing_{false};
    bool scrub_drag_active_ = false;
    int64_t last_scrub_target_ = -1;
    unsigned scrub_log_tick_ = 0;
    std::chrono::steady_clock::time_point scrub_start_{};
    int64_t scrub_first_ = 0;
    std::uint64_t scrub_previews_ = 0;
    std::uint64_t scrub_preview_hits_ = 0;
    std::uint64_t scrub_preview_misses_ = 0;
    std::uint64_t scrub_preview_evictions_ = 0;
    double scrub_preview_ms_sum_ = 0.0;
    double scrub_preview_ms_max_ = 0.0;
    std::chrono::steady_clock::time_point play_t0_{};
    bool play_armed_ = false;
    std::uint64_t aud_baseline_frames_ = 0;
    bool aud_armed_ = false;
    std::chrono::steady_clock::time_point seek_arm_t0_{};
    bool seek_present_armed_ = false;
    std::int64_t seek_present_target_ = 0;
    std::atomic<bool> scrub_audio_enabled_{true};
    bool scrub_audio_open_ = false;
    std::atomic<bool> play_pause_intent_{false};
    std::atomic<int64_t> current_frame_{-1};
    std::atomic<int64_t> total_frames_{-1};
    std::atomic<double> fps_{0.0};

    using Clock = std::chrono::steady_clock;
    Clock::time_point next_present_{};
    Clock::time_point last_present_ts_{};
    int64_t contig_delta_ = 0;
    int64_t present_ready_hits_ = 0;
    int64_t present_inline_ = 0;
    int64_t drop_events_ = 0;
    int64_t drop_frames_ = 0;
    int64_t cap_events_ = 0;
    int64_t last_ready_depth_ = 0;

    AudioOutput audio_out_;
    AudioPipeline audio_;
    SonicSync sonicsync_;
};

}