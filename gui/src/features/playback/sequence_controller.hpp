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

    // Swap the project the worker decodes from. `initial_frame` re-anchors the
    // playhead; pass -1 to preserve the current one (edit snapshots must not
    // reset the playhead — only a fresh open/new passes 0).
    void set_project(std::shared_ptr<const canvas::core::Project> project, int64_t initial_frame = -1);
    void add_media(const canvas::core::MediaEntry& entry);
    void play();
    void pause();
    void toggle_play_pause();
    void seek(int64_t frame_number);
    // Fast low-resolution scrub preview: decodes the frame at a reduced size for
    // responsive scrubbing and does NOT write it into the full-res frame cache.
    // Committed positions (release, play, transport) should use seek() to get the
    // full-resolution frame.
    void seek_preview(int64_t frame_number);
    // Mark the start of a scrub drag. While scrubbing (even during playback)
    // seek_preview() takes the fast low-res path and deliberately does NOT tear
    // down/rebuild the live audio pipe per mouse-move. The definitive audio
    // re-anchor + crisp-frame commit happens once on release via seek(). Call
    // begin_scrub() on grab, end_scrub() on release.
    void begin_scrub();
    void end_scrub();
    void step(int64_t delta);
    // Enable/disable audible scrubbing (sound grains while dragging the playhead).
    void set_scrub_audio_enabled(bool on) { scrub_audio_enabled_.store(on); }
    [[nodiscard]] bool scrub_audio_enabled() const { return scrub_audio_enabled_.load(); }

    // Monitoring volume / mute, forwarded to the audio output. Volume is [0,1];
    // setting volume un-mutes. Mute silences without losing the volume.
    void set_volume(float volume) { audio_out_.set_volume(volume); }
    [[nodiscard]] float volume() const { return audio_out_.volume(); }
    void set_muted(bool muted) { audio_out_.set_muted(muted); }
    [[nodiscard]] bool muted() const { return audio_out_.muted(); }
    // Temporary monitoring "dim" (dips the level below the set volume).
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
    enum class Command { SetProject, AddMedia, Play, Pause, Seek, SeekPreview, Step, Stop };
    struct Request {
        Command command = Command::Stop;
        int64_t arg = 0;
        std::shared_ptr<const canvas::core::Project> project;
        canvas::core::MediaEntry media;
    };

    void worker_loop();
    void push(Request request);
    void handle_set_project(std::shared_ptr<const canvas::core::Project> project, int64_t initial_frame);
    void handle_add_media(const canvas::core::MediaEntry& entry);
    void handle_play();
    void handle_seek(int64_t frame_number);
    void handle_seek_preview(int64_t frame_number);
    void warm_lookahead(int64_t start_frame);
    double media_rate_at(int64_t seq_frame) const;
    void reset_ready();
    void fill_lookahead(int64_t base);
    void present_next();
    canvas::core::RenderFramePtr frame_for_playhead(int64_t seq_frame);
    canvas::core::RenderFramePtr frame_for_playhead_preview(int64_t seq_frame, int max_dim);

    // Audio helpers.
    // All audible decode/feed/anchors live in AudioPipeline (Qt-free); the
    // controller forwards playhead state and reads back the pipeline's audible
    // position for A/V sync.

    std::shared_ptr<const canvas::core::Project> project_;

    // Qt-free video decode front-end (slot caches, preview LRU, HW device).
    TimelineDecoder decoder_;

    std::thread worker_;
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<Request> queue_;

    // Decode-ahead lookahead buffer so presentation pops an already-ready frame
    // instead of decoding inline. Pacing/sync constants (kLookahead,
    // kScrubPrecache, kPreviewMaxDim, kAudioLeadMs) live in sync_constants.hpp
    // so they are unit-testable.
    std::deque<canvas::core::RenderFramePtr> ready_;
    int64_t ready_base_ = -1;
    std::atomic<bool> playing_{false};
    std::atomic<bool> stopping_{false};
    // True while a scrub drag is active (see begin_scrub/end_scrub); read on the
    // worker by handle_seek_preview to avoid per-move audio rewinds during a
    // playback scrub.
    std::atomic<bool> scrubbing_{false};
    // True once begin_scrub() has run within the current drag; cleared by
    // end_scrub(). Makes repeated begin_scrub() calls idempotent so drag
    // telemetry/audio-open only happen on the first move.
    bool scrub_drag_active_ = false;
    int64_t last_scrub_target_ = -1;
    unsigned scrub_log_tick_ = 0;
    std::chrono::steady_clock::time_point scrub_start_{};
    int64_t scrub_first_ = 0;
    // Audible-scrub preference. When true, each settled scrub position writes a
    // short audio grain decoded from the media at that time, so scrubbing "sounds
    // out" the media position. Independent of normal playback audio.
    std::atomic<bool> scrub_audio_enabled_{true};
    bool scrub_audio_open_ = false;
    // UI-thread-side play/pause intent. toggle_play_pause() must read THIS
    // (updated synchronously on every play()/pause()/toggle call) rather than
    // the async `playing_`, which lags behind queued commands. Otherwise after a
    // scrub-triggered pause() whose command is still queued, pressing Play sees
    // playing_==true, issues another Pause -> silence, and handle_play() never
    // runs.
    std::atomic<bool> play_pause_intent_{false};
    std::atomic<int64_t> current_frame_{-1};
    std::atomic<int64_t> total_frames_{-1};
    std::atomic<double> fps_{0.0};

    using Clock = std::chrono::steady_clock;
    Clock::time_point next_present_{};
    // Wall-clock of the most recent present, for true per-frame cadence tracking
    // (the [play] health log uses it so cadence_ms isn't just the throttle tick).
    Clock::time_point last_present_ts_{};
    // Contiguous frame-walks since the last [play] health sample (drives
    // fps_window without the stale-sample `non-contiguous` artifact).
    int64_t contig_delta_ = 0;

    // Audio output state. The concrete sink is owned here (volume/mute/dim and
    // scrub-end telemetry read it directly) and injected into AudioPipeline,
    // which does all audible decode/feed through the AudioSink interface.
    AudioOutput audio_out_;
    AudioPipeline audio_;
    // A/V clock reconciliation (MLT "audio rides with its frame" model). Owns the
    // seek-hold gate and the master-clock drop/hold policy; see sonicsync.hpp.
    SonicSync sonicsync_;
};

}  // namespace canvas::gui
