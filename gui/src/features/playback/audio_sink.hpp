#pragma once

#include <cstddef>
#include <cstdint>

namespace canvas::gui {

class AudioSink {
public:
    virtual ~AudioSink() = default;

    virtual bool open(int sample_rate, int channels) = 0;
    virtual void close() = 0;
    [[nodiscard]] virtual bool is_open() const = 0;

    virtual bool write_float(const float* data, int frames) = 0;
    virtual void flush() = 0;
    virtual bool reposition_enqueue(const float* data, int frames) = 0;
    [[nodiscard]] virtual std::size_t pending_frames() const = 0;

    virtual void set_hold_active(bool on) = 0;

    [[nodiscard]] virtual uint64_t stat_written_frames() const = 0;
    [[nodiscard]] virtual uint64_t audible_position_frames() const = 0;
    virtual void log_pipeline_stats(const char* tag) const = 0;
};

}
