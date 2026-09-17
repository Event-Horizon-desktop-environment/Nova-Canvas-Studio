#ifndef CANVAS_TESTS_FAKE_AUDIO_SINK_HPP
#define CANVAS_TESTS_FAKE_AUDIO_SINK_HPP

#include "features/playback/audio_sink.hpp"

#include <cstdint>
#include <vector>

namespace canvas::gui::test {

struct FakeAudioSink : AudioSink {
    int rate = 0;
    int channels = 0;
    bool open_ = false;
    bool hold_ = false;
    std::vector<float> all_;
    std::vector<int> writes_;
    std::vector<float> queue_;
    uint64_t written_total_ = 0;
    uint64_t latency_ = 0;

    bool open(int sample_rate, int channels_) override {
        rate = sample_rate;
        channels = channels_;
        open_ = true;
        queue_.clear();
        return true;
    }
    void close() override {
        open_ = false;
        queue_.clear();
    }
    bool is_open() const override { return open_; }

    bool write_float(const float* data, int frames) override {
        if (!open_ || frames <= 0 || !data) return false;
        const std::size_t n = static_cast<std::size_t>(frames) * channels;
        all_.insert(all_.end(), data, data + n);
        queue_.insert(queue_.end(), data, data + n);
        written_total_ += static_cast<uint64_t>(frames);
        writes_.push_back(frames);
        return true;
    }
    void flush() override { queue_.clear(); }
    bool reposition_enqueue(const float* data, int frames) override {
        if (frames <= 0 || !data) return false;
        const std::size_t n = static_cast<std::size_t>(frames) * channels;
        queue_.assign(data, data + n);
        return true;
    }
    std::size_t pending_frames() const override { return queue_.size() / channels; }
    void set_hold_active(bool on) override { hold_ = on; }
    uint64_t stat_written_frames() const override { return written_total_; }
    uint64_t audible_position_frames() const override {
        return written_total_ >= latency_ ? written_total_ - latency_ : 0;
    }
    void log_pipeline_stats(const char*) const override {}
};

}

#endif
