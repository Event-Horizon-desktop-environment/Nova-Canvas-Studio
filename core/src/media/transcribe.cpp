#include "canvas/core/media/transcribe.hpp"

#include "canvas/core/media/audio_decoder.hpp"
#include "canvas/core/media/frame.hpp"

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <sys/stat.h>

#include "whisper.h"

namespace canvas::core::transcribe {

namespace {

bool is_regular_file(const std::string& p) {
    struct stat st {};
    return ::stat(p.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

std::string cache_dir() {
    if (const char* x = std::getenv("XDG_CACHE_HOME"); x && *x) return x;
    if (const char* h = std::getenv("HOME"); h && *h) return std::string(h) + "/.cache";
    return "/tmp";
}

void progress_trampoline(struct whisper_context*, struct whisper_state*,
                         int progress, void* user_data) {
    auto* p = static_cast<Progress*>(user_data);
    if (!p) return;
    if (progress < 0) progress = 0;
    if (progress > 100) progress = 100;
    p->percent.store(progress, std::memory_order_relaxed);
}

bool abort_trampoline(void* user_data) {
    const auto* p = static_cast<const Progress*>(user_data);
    return p && p->cancel.load(std::memory_order_relaxed);
}

void finish_progress(Progress* p) {
    if (!p) return;
    p->percent.store(100, std::memory_order_relaxed);
    p->stage.store(Progress::Stage::kFinished, std::memory_order_relaxed);
}

}

std::string resolve_model_path(const std::string& override) {
    if (!override.empty() && is_regular_file(override)) return override;
    if (const char* env = std::getenv("CANVAS_WHISPER_MODEL"); env && *env && is_regular_file(env))
        return env;
    const std::string dir = cache_dir() + "/nova-canvas/whisper/";
    for (const char* name : {"ggml-base.bin", "ggml-small.bin", "ggml-medium.bin"}) {
        const std::string candidate = dir + name;
        if (is_regular_file(candidate)) return candidate;
    }
    return {};
}

Report transcribe_pcm_16k(const float* pcm, std::size_t n_samples,
                          const std::string& model_path, const Options& opts) {
    Report out;
    if (!pcm || n_samples == 0) {
        out.result = Result::kBadInput;
        out.error = "pcm pointer and/or size invalid";
        finish_progress(opts.progress);
        return out;
    }
    if (n_samples > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        out.result = Result::kBadInput;
        out.error = "pcm longer than whisper's int sample count";
        finish_progress(opts.progress);
        return out;
    }
    if (!is_regular_file(model_path)) {
        out.result = Result::kNoModel;
        out.error = "model file not found: " + model_path;
        return out;
    }
    out.audio_seconds = static_cast<double>(n_samples) / 16000.0;
    const auto wall_start = std::chrono::steady_clock::now();

    if (opts.progress)
        opts.progress->stage.store(Progress::Stage::kLoadingModel,
                                   std::memory_order_relaxed);
    whisper_context_params cparams = whisper_context_default_params();
    cparams.use_gpu = false;
    whisper_context* ctx = whisper_init_from_file_with_params(model_path.c_str(), cparams);
    if (!ctx) {
        out.result = Result::kCouldNotInit;
        out.error = "whisper_init_from_file_with_params failed for: " + model_path;
        out.wall_seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() -
                                                         wall_start)
                               .count();
        finish_progress(opts.progress);
        return out;
    }

    whisper_full_params params = whisper_full_default_params(WHISPER_SAMPLING_GREEDY);
    params.print_progress = opts.verbose;
    params.print_realtime = false;
    params.n_threads = opts.threads > 0 ? opts.threads : 1;
    params.language = (opts.language && *opts.language) ? opts.language : "auto";
    params.max_len = opts.max_len;
    params.max_tokens = opts.max_tokens;
    if (opts.progress) {
        params.progress_callback = progress_trampoline;
        params.progress_callback_user_data = opts.progress;
        params.abort_callback = abort_trampoline;
        params.abort_callback_user_data = opts.progress;
        opts.progress->stage.store(Progress::Stage::kTranscribing,
                                   std::memory_order_relaxed);
        opts.progress->percent.store(0, std::memory_order_relaxed);
    }

    const int rc = whisper_full(ctx, params, pcm, static_cast<int>(n_samples));
    out.wall_seconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - wall_start).count();
    if (rc != 0) {
        out.result = (opts.progress && opts.progress->cancel.load(std::memory_order_relaxed))
                         ? Result::kAborted
                         : Result::kRunFailed;
        out.error = out.result == Result::kAborted
                        ? "transcription aborted by cancel request"
                        : "whisper_full rc=" + std::to_string(rc);
        whisper_free(ctx);
        finish_progress(opts.progress);
        return out;
    }
    finish_progress(opts.progress);

    const int n_segments = whisper_full_n_segments(ctx);
    out.cues.reserve(static_cast<std::size_t>(n_segments));
    for (int i = 0; i < n_segments; ++i) {
        transcript::Segment s;
        s.start_ms = whisper_full_get_segment_t0(ctx, i) * 10;
        s.end_ms = whisper_full_get_segment_t1(ctx, i) * 10;
        s.text = transcript::clean_segment_text(whisper_full_get_segment_text(ctx, i));
        if (!s.text.empty()) out.cues.push_back(std::move(s));
    }
    whisper_free(ctx);
    out.result = Result::kDone;
    return out;
}

namespace {

constexpr std::size_t kMaxReadSamples = 16'000u * 60u * 60u * 4u;

}

Report transcribe_file(const std::string& path, const std::string& model_path,
                       const Options& opts) {
    Report out;

    if (opts.progress) {
        opts.progress->stage.store(Progress::Stage::kDecodingAudio,
                                   std::memory_order_relaxed);
        opts.progress->percent.store(0, std::memory_order_relaxed);
    }
    canvas::core::AudioDecoder audio;
    if (!audio.open(path)) {
        out.result = Result::kBadInput;
        out.error = "audio open failed: " + path;
        return out;
    }

    const int in_channels = audio.source_channels();
    if (in_channels <= 0) {
        out.result = Result::kBadInput;
        out.error = "audio stream has no channels: " + path;
        return out;
    }

    std::vector<float> mono;
    mono.reserve(16'000u * 64u);
    constexpr int kChunkFrames = 16'384;
    for (;;) {
        canvas::core::AudioChunkPtr chunk = audio.decode(static_cast<int64_t>(mono.size()),
                                                         kChunkFrames, 16'000);
        if (!chunk || chunk->samples.empty()) break;
        if (audio.at_stream_end()) break;
        const std::size_t frames = chunk->samples.size() / static_cast<std::size_t>(chunk->channels);
        if (frames == 0) break;
        if (mono.size() + frames > kMaxReadSamples) {
            out.result = Result::kBadInput;
            out.error = "audio too long (>4h at 16 kHz): " + path;
            return out;
        }
        for (std::size_t i = 0; i < frames; ++i) {
            float sum = 0.0f;
            for (int c = 0; c < chunk->channels; ++c) {
                sum += chunk->samples[i * static_cast<std::size_t>(chunk->channels) +
                                      static_cast<std::size_t>(c)];
            }
            mono.push_back(sum / static_cast<float>(chunk->channels));
        }
    }

    if (mono.empty()) {
        out.result = Result::kBadInput;
        out.error = "no audio decoded: " + path;
        return out;
    }

    return transcribe_pcm_16k(mono.data(), mono.size(), model_path, opts);
}

}
