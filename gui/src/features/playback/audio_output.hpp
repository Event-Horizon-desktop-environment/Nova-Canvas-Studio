#pragma once

#include <atomic>
#include <cstdint>
#include <memory>

#include "audio_sink.hpp"

namespace canvas::gui {

class AudioOutput : public AudioSink {
public:
    AudioOutput();
    ~AudioOutput() override;
    AudioOutput(const AudioOutput&) = delete;
    AudioOutput& operator=(const AudioOutput&) = delete;

    bool open(int sample_rate, int channels) override;
    void close() override;
    [[nodiscard]] bool is_open() const override { return open_; }

    bool write_float(const float* data, int frames) override;
    void flush() override;
    bool reposition_enqueue(const float* data, int frames) override;
    [[nodiscard]] std::size_t pending_frames() const override;

    void set_volume(float volume);
    [[nodiscard]] float volume() const { return volume_.load(); }
    void set_muted(bool muted);
    [[nodiscard]] bool muted() const { return muted_.load(); }
    void set_dimmed(bool dimmed);
    [[nodiscard]] bool dimmed() const { return dimmed_.load(); }
    [[nodiscard]] float effective_volume() const;

    void set_hold_active(bool on) override;

    uint64_t stat_enqueued_frames() const;
    uint64_t stat_dropped_frames() const;
    uint64_t stat_written_frames() const override;
    uint64_t stat_write_errors() const;
    [[nodiscard]] uint64_t stat_silence_holds() const;
    [[nodiscard]] uint64_t stat_silence_hold_frames() const;
    [[nodiscard]] uint64_t stat_xruns() const;

    uint64_t audible_position_frames() const override;
    void log_pipeline_stats(const char* tag) const override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    bool open_ = false;
    int write_log_ticks_ = 0;
    std::atomic<float> volume_{0.8f};
    std::atomic<bool> muted_{false};
    std::atomic<bool> dimmed_{false};
};

}
