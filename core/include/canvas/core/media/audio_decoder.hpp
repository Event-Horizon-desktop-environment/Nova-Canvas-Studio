#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "canvas/core/media/frame.hpp"

namespace canvas::core {

// Decodes a single media file's audio stream to interleaved float PCM at an
// arbitrary output sample rate. It owns an independent AVFormatContext and
// decoder state, so it never disturbs concurrent video decoding of the same
// file (a separate VideoDecoder may be open on the same path).
class AudioDecoder {
public:
    AudioDecoder();
    ~AudioDecoder();
    AudioDecoder(const AudioDecoder&) = delete;
    AudioDecoder& operator=(const AudioDecoder&) = delete;

    bool open(const std::string& path);
    void close();

    [[nodiscard]] bool has_audio() const;
    [[nodiscard]] int source_sample_rate() const;
    [[nodiscard]] int source_channels() const;
    // Total duration in seconds of the opened media (used to derive a frame
    // count for media-pool import). Prefers the container duration; falls back
    // to the audio stream's own duration when the container omits one. Returns
    // 0.0 when no media is open or the duration is unknown.
    [[nodiscard]] double duration_seconds() const;

    // Decodes up to `max_frames` interleaved float frames at `out_sample_rate`
    // starting at `start_sample`. `start_sample` is in *output* sample units
    // (seconds * out_sample_rate). Returns empty when no audio is available.
    AudioChunkPtr decode(int64_t start_sample, int max_frames, int out_sample_rate);
    void seek(int64_t start_sample, int out_sample_rate);
    void reset();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}
