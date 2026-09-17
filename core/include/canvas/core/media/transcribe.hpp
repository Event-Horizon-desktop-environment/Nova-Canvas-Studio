#pragma once

#include "canvas/core/media/transcript.hpp"

#include <atomic>
#include <cstddef>
#include <string>
#include <vector>

namespace canvas::core::transcribe {

enum class Result {
    kDone,
    kNoModel,
    kBadInput,
    kCouldNotInit,
    kRunFailed,
    kAborted,
};

struct Progress {
    enum class Stage : int {
        kDecodingAudio = 0,
        kLoadingModel,
        kTranscribing,
        kFinished,
    };
    std::atomic<int> percent{0};
    std::atomic<Stage> stage{Stage::kDecodingAudio};
    std::atomic<bool> cancel{false};
};

struct Options {
    int threads = 4;
    int32_t max_len = 0;
    int32_t max_tokens = 0;
    const char* language = nullptr;
    bool verbose = false;
    Progress* progress = nullptr;
};

struct Report {
    Result result = Result::kNoModel;
    std::vector<transcript::Segment> cues;
    std::string error;
    double audio_seconds = 0.0;
    double wall_seconds = 0.0;
};

[[nodiscard]] std::string resolve_model_path(const std::string& override = {});

[[nodiscard]] Report transcribe_pcm_16k(const float* pcm, std::size_t n_samples,
                                        const std::string& model_path,
                                        const Options& opts = {});

[[nodiscard]] Report transcribe_file(const std::string& path,
                                     const std::string& model_path,
                                     const Options& opts = {});

}
