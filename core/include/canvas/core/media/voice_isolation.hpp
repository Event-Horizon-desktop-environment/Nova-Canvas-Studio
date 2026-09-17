#pragma once

#include <cstdint>
#include <map>
#include <memory>

namespace canvas::core {

enum class VoiceIsolationMode : uint8_t {
    None = 0,
    RnNoise = 1,
    DeepFilterNet = 2
};

[[nodiscard]] const char* voice_isolation_mode_name(VoiceIsolationMode mode) noexcept;

[[nodiscard]] bool voice_isolation_supported(VoiceIsolationMode mode) noexcept;

class VoiceIsolation {
public:
    static constexpr int kSampleRate = 48000;
    static constexpr int kFrameSize = 480;

    VoiceIsolation();
    ~VoiceIsolation();
    VoiceIsolation(const VoiceIsolation&) = delete;
    VoiceIsolation& operator=(const VoiceIsolation&) = delete;
    VoiceIsolation(VoiceIsolation&&) noexcept;
    VoiceIsolation& operator=(VoiceIsolation&&) noexcept;

    [[nodiscard]] int process(int sample_rate, int channels, float* samples, int num_frames);

    void reset();

private:
    struct State;
    static bool ensure_networks(State& state, int channels);
    std::unique_ptr<State> state_;
};

class VoiceIsolationBank {
public:
    [[nodiscard]] int tick(std::uint64_t clip_id, VoiceIsolationMode mode, int sample_rate,
                           int channels, float* samples, int num_frames);

    void drop();
    void drop(std::uint64_t clip_id);
    void clear();

private:
    struct Entry {
        VoiceIsolation engine;
        VoiceIsolationMode mode = VoiceIsolationMode::None;
    };
    std::map<std::uint64_t, Entry> entries_;
};

}
