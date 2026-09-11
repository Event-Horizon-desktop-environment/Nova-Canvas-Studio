#pragma once

// Per-clip AI voice isolation: a small headless seam in canvas_core so
// playback (AudioPipeline) and export (RenderSession) apply the SAME per-clip
// denoising before the clip's gains/mix. Qt-free by construction.
//
// Shipped engine: RNNoise (vendored xiph/rnnoise, BSD-3-Clause) — a real-time
// recurrent-neural-network speech noise SUPPRESSOR. It strips stationary
// background noise (fan/hum/room tone/keyboard) from speech; it does NOT
// separate speech layered over music or over a second voice. Operates on
// 48 kHz mono/stereo interleaved float PCM (one network state per channel).
//
// The DeepFilterNet mode is the future voice-from-music number: it is the
// seam's second engine slot but is not built in this tree
// (voice_isolation_supported() == false), so the per-clip dropdown in the
// Inspector still lists the choice and disables it.

#include <cstdint>
#include <map>
#include <memory>

namespace canvas::core {

enum class VoiceIsolationMode : uint8_t {
    None = 0,         // no isolation
    RnNoise = 1,      // real-time speech noise suppression (shipped)
    DeepFilterNet = 2 // voice-from-music separation (not built yet)
};

// Human-readable engine name ("None" / "RNNoise..." / "DeepFilterNet...").
[[nodiscard]] const char* voice_isolation_mode_name(VoiceIsolationMode mode) noexcept;

// True when the engine is compiled into this build AND safe to run. Only
// RnNoise qualifies today; DeepFilterNet reports false (seam reserved).
[[nodiscard]] bool voice_isolation_supported(VoiceIsolationMode mode) noexcept;

// Streaming RNNoise wrapper. Independent of project/timeline types so it can
// live on the playback worker and in the export session without coupling.
//
// Contract:
//  - sample_rate must be 48000; channels must be 1 or 2 (interleaved). Any
//    other configuration is passed through untouched (process() writes nothing
//    and returns 0).
//  - process() is a STREAM filter: 480-sample frames are buffered internally,
//    so the number of frames written for one call can be less than `num_frames`
//    while the network primes, and up to `num_frames` once steady. Callers must
//    consume the returned count and mix only those frames (a short head of a
//    few ms is RNNoise's own lookahead). Output replaces input in place.
//  - reset() drops ALL network state (use it when the audio position jumps, so
//    no stale GRU memory bleeds across the seek). Held-over partial frames (up
//    to 479) are discarded by reset; that is the unavoidable streaming-filter
//    tail and matches RNNoise's own 10 ms lookahead.
class VoiceIsolation {
public:
    static constexpr int kSampleRate = 48000;
    static constexpr int kFrameSize = 480;  // rnnoise_get_frame_size(), 10 ms

    VoiceIsolation();
    ~VoiceIsolation();
    VoiceIsolation(const VoiceIsolation&) = delete;
    VoiceIsolation& operator=(const VoiceIsolation&) = delete;
    VoiceIsolation(VoiceIsolation&&) noexcept;
    VoiceIsolation& operator=(VoiceIsolation&&) noexcept;

    // Denoise interleaved `samples` (num_frames*channels floats) in place.
    // Returns the number of frames written (<= num_frames) at 48 kHz.
    [[nodiscard]] int process(int sample_rate, int channels, float* samples, int num_frames);

    void reset();

private:
    struct State;
    static bool ensure_networks(State& state, int channels);
    std::unique_ptr<State> state_;
};

// Small per-clip registry so the realtime and export paths can run a network
// continuously across calls (keyed by ClipId). Use tick() for every clip; the
// correct per-clip engine is looked up from the clip's VoiceIsolationMode.
// drop() must be called when the timeline position jumps out of sequence
// (seek/rewind) so GRU state never carries across a discontinuity.
class VoiceIsolationBank {
public:
    // Denoise `samples` in place for `clip`. Returns frames written (<=
    // num_frames) — equal to num_frames when no isolation is active or the mode
    // is unsupported/rate mismatched (pass-through), matching the engine law.
    [[nodiscard]] int tick(std::uint64_t clip_id, VoiceIsolationMode mode, int sample_rate,
                           int channels, float* samples, int num_frames);

    void drop();              // reset all per-clip state
    void drop(std::uint64_t clip_id);  // reset one clip's engine state
    void clear(); // release every per-clip state object

private:
    struct Entry {
        VoiceIsolation engine;
        VoiceIsolationMode mode = VoiceIsolationMode::None;
    };
    std::map<std::uint64_t, Entry> entries_;
};

}  // namespace canvas::core