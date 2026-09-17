#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "canvas/core/media/frame.hpp"

namespace canvas::core {

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
    [[nodiscard]] double duration_seconds() const;

    [[nodiscard]] bool at_stream_end() const;

    AudioChunkPtr decode(int64_t start_sample, int max_frames, int out_sample_rate);
    void seek(int64_t start_sample, int out_sample_rate);
    void reset();

    [[nodiscard]] std::uint64_t resync_count() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}
