#include "canvas/core/media/voice_isolation.hpp"

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <vector>

#include <rnnoise.h>

namespace canvas::core {

const char* voice_isolation_mode_name(const VoiceIsolationMode mode) noexcept {
    switch (mode) {
        case VoiceIsolationMode::None: return "None";
        case VoiceIsolationMode::RnNoise: return "RNNoise Noise Suppression";
        case VoiceIsolationMode::DeepFilterNet: return "DeepFilterNet Voice Isolation";
    }
    return "Unknown";
}

bool voice_isolation_supported(const VoiceIsolationMode mode) noexcept {
    // Only RNNoise is compiled into this tree. DeepFilterNet is the reserved
    // voice-from-music engine slot (onnxruntime + model, not yet shipped).
    return mode == VoiceIsolationMode::RnNoise;
}

// Fixed-size scratch for one network frame (480 floats) lives in State as
// pre-sized vectors, avoiding per-call allocations in the realtime path
// (AudioPipeline::write_mixed runs at device cadence).

struct VoiceIsolation::State {
    DenoiseState* left = nullptr;  // mono uses only `left`
    DenoiseState* right = nullptr; // second channel when channels == 2
    std::vector<float> lane_left;  // deinterleaved 480-frame input, L
    std::vector<float> lane_right; // deinterleaved 480-frame input, R
    std::vector<float> denoised_left;
    std::vector<float> denoised_right;
    // Pending (not yet consumed) input, interleaved, front-aligned.
    std::vector<float> in_fifo;
    // Denoised output queued for the caller, interleaved, front-aligned.
    std::vector<float> out_fifo;
};

VoiceIsolation::VoiceIsolation() : state_(std::make_unique<State>()) {}

VoiceIsolation::~VoiceIsolation() {
    if (state_->right) rnnoise_destroy(state_->right);
    if (state_->left) rnnoise_destroy(state_->left);
}

VoiceIsolation::VoiceIsolation(VoiceIsolation&& other) noexcept
    : state_(std::move(other.state_)) {}

VoiceIsolation& VoiceIsolation::operator=(VoiceIsolation&& other) noexcept {
    if (this != &other) state_ = std::move(other.state_);
    return *this;
}

// Ensures the per-channel network state exists. RNNoise's default model is
// compiled in (rnn_data.c), so no model file is required.
bool VoiceIsolation::ensure_networks(State& s, int channels) {
    if (!s.left) {
        s.left = rnnoise_create(nullptr);
        if (!s.left) return false;
    }
    if (channels == 2 && !s.right) {
        s.right = rnnoise_create(nullptr);
        if (!s.right) return false;
    }
    return true;
}

int VoiceIsolation::process(const int sample_rate, const int channels, float* const samples,
                            const int num_frames) {
    if (sample_rate != kSampleRate || (channels != 1 && channels != 2) || num_frames <= 0) {
        return 0;
    }
    State& s = *state_;
    if (!ensure_networks(s, channels)) return 0;

    const std::size_t ch = static_cast<std::size_t>(channels);
    const std::size_t new_floats = static_cast<std::size_t>(num_frames) * ch;

    // 1. Append the new input to the pending FIFO.
    const std::size_t in_before = s.in_fifo.size();
    s.in_fifo.resize(in_before + new_floats);
    std::memcpy(s.in_fifo.data() + in_before, samples, new_floats * sizeof(float));
    const std::size_t in_frames = (in_before + new_floats) / ch;

    // 2. Denoise every complete 480-frame block.
    std::size_t pending = in_frames;
    while (pending >= static_cast<std::size_t>(kFrameSize)) {
        const float* block = s.in_fifo.data();
        s.lane_left.resize(static_cast<std::size_t>(kFrameSize));
        for (int i = 0; i < kFrameSize; ++i) {
            s.lane_left[static_cast<std::size_t>(i)] = block[static_cast<std::size_t>(i) * ch];
        }
        if (channels == 2) {
            s.lane_right.resize(static_cast<std::size_t>(kFrameSize));
            for (int i = 0; i < kFrameSize; ++i) {
                s.lane_right[static_cast<std::size_t>(i)] =
                    block[static_cast<std::size_t>(i) * ch + 1];
            }
        }
        s.denoised_left.resize(static_cast<std::size_t>(kFrameSize));
        rnnoise_process_frame(s.left, s.denoised_left.data(), s.lane_left.data());
        if (channels == 2) {
            s.denoised_right.resize(static_cast<std::size_t>(kFrameSize));
            rnnoise_process_frame(s.right, s.denoised_right.data(), s.lane_right.data());
        }
        for (int i = 0; i < kFrameSize; ++i) {
            s.out_fifo.push_back(s.denoised_left[static_cast<std::size_t>(i)]);
            if (channels == 2) {
                s.out_fifo.push_back(s.denoised_right[static_cast<std::size_t>(i)]);
            }
        }
        // Consume the block.
        const std::size_t consumed_floats =
            static_cast<std::size_t>(kFrameSize) * ch;
        std::memmove(s.in_fifo.data(), s.in_fifo.data() + consumed_floats,
                     (s.in_fifo.size() - consumed_floats) * sizeof(float));
        s.in_fifo.resize(s.in_fifo.size() - consumed_floats);
        pending -= static_cast<std::size_t>(kFrameSize);
    }

    // 3. Emit up to `num_frames` from the denoised backlog (in place).
    const std::size_t emit_floats =
        std::min(new_floats, s.out_fifo.size());
    if (emit_floats > 0) {
        std::memcpy(samples, s.out_fifo.data(), emit_floats * sizeof(float));
        std::memmove(s.out_fifo.data(), s.out_fifo.data() + emit_floats,
                     (s.out_fifo.size() - emit_floats) * sizeof(float));
        s.out_fifo.resize(s.out_fifo.size() - emit_floats);
    }
    return static_cast<int>(emit_floats / ch);
}

void VoiceIsolation::reset() {
    if (state_->right) {
        rnnoise_destroy(state_->right);
        state_->right = nullptr;
    }
    if (state_->left) {
        rnnoise_destroy(state_->left);
        state_->left = nullptr;
    }
    state_->in_fifo.clear();
    state_->out_fifo.clear();
}

int VoiceIsolationBank::tick(const std::uint64_t clip_id, const VoiceIsolationMode mode,
                             const int sample_rate, const int channels, float* const samples,
                             const int num_frames) {
    if (mode == VoiceIsolationMode::None || !voice_isolation_supported(mode) ||
        sample_rate != VoiceIsolation::kSampleRate || (channels != 1 && channels != 2)) {
        // Pass through untouched — AND drop any stale per-clip network state:
        // an engine toggle off/on must start the next run from a fresh GRU
        // instead of resuming whatever the pre-toggle stream was doing.
        entries_.erase(clip_id);
        return num_frames;
    }
    Entry& entry = entries_[clip_id];
    if (entry.mode != mode) {
        // Mode flipped for an existing clip: a fresh network is correct (the
        // edit could also have changed the content under the same id).
        entry.engine.reset();
        entry.mode = mode;
    }
    return entry.engine.process(sample_rate, channels, samples, num_frames);
}

void VoiceIsolationBank::drop() {
    for (auto& kv : entries_) kv.second.engine.reset();
}

void VoiceIsolationBank::drop(const std::uint64_t clip_id) {
    const auto it = entries_.find(clip_id);
    if (it != entries_.end()) it->second.engine.reset();
}

void VoiceIsolationBank::clear() { entries_.clear(); }

}  // namespace canvas::core