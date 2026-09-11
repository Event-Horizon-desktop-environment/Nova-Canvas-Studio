#pragma once

// Abstract audio sink consumed by AudioPipeline. Lives in its own Qt-free header
// so AudioPipeline (and its future headless tests with a mock sink) never has to
// touch the concrete AudioOutput, whose implementation pulls in ALSA/PipeWire.
//
// The concrete AudioOutput implements this interface 1:1.

#include <cstddef>
#include <cstdint>

namespace canvas::gui {

class AudioSink {
public:
    virtual ~AudioSink() = default;

    virtual bool open(int sample_rate, int channels) = 0;
    virtual void close() = 0;
    [[nodiscard]] virtual bool is_open() const = 0;

    // Writes `frames` interleaved frames (frames * channels floats total).
    virtual bool write_float(const float* data, int frames) = 0;
    virtual void flush() = 0;
    virtual bool reposition_enqueue(const float* data, int frames) = 0;
    [[nodiscard]] virtual std::size_t pending_frames() const = 0;

    // Topped-up silence between real chunks while true (live playback).
    virtual void set_hold_active(bool on) = 0;

    [[nodiscard]] virtual uint64_t stat_written_frames() const = 0;
    // Device underruns since open (ALSA/PipeWire xruns). A starvation glitch
    // never appears in the sample stream the pipeline writes, so the audible
    // "pop/click" must be counted here or it is invisible to the mix audit.
    [[nodiscard]] virtual uint64_t stat_xruns() const = 0;
    // Frames currently AUDIBLE at the speaker (written minus device latency).
    [[nodiscard]] virtual uint64_t audible_position_frames() const = 0;
    virtual void log_pipeline_stats(const char* tag) const = 0;
};

}  // namespace canvas::gui