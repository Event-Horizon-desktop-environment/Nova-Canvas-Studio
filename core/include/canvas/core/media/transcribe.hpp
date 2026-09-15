#pragma once

// Qt-free headless transcription engine seam over whisper.cpp (FetchedContent,
// pinned at a tag — see core/CMakeLists.txt). The module owns the three things
// nothing else should: model-path resolution, the whisper context lifecycle,
// and the whisper 10 ms tick -> millisecond cue conversion. The transcript
// format laws live in transcript.hpp and are engine-independent.
//
// Feeding is PCM-only: the caller supplies 16 kHz mono float samples in
// [-1, 1]; the decode + 16 kHz mono resample glue lives in a thin wrapper
// (GUI-side Deliver action / media action) that stays Qt-free too. The engine
// forces the CPU backend (use_gpu = false) so a background transcription job
// is portable and never depends on the CUDA runtime.

#include "canvas/core/media/transcript.hpp"

#include <atomic>
#include <cstddef>
#include <string>
#include <vector>

namespace canvas::core::transcribe {

enum class Result {
    kDone,          // model ran; `cues` may still be empty (no speech detected)
    kNoModel,       // no model file at the resolved path, or path is not a file
    kBadInput,      // null / zero-length pcm, or too long to hand to whisper
    kCouldNotInit,  // whisper failed to load the model (corrupt / wrong file)
    kRunFailed,     // whisper_full returned a nonzero code
    kAborted,       // the Progress::cancel flag was set mid-run (UI Cancel)
};

// Live progress channel between the engine and a (usually GUI) progress popup.
// The engine writes `percent`/`stage` from the transcription worker thread and
// polls `cancel` through whisper's abort hook; the UI side only ever touches
// atomics, so no locking discipline is needed. The pointed-to Progress must
// outlive the transcribe call.
struct Progress {
    enum class Stage : int {
        kDecodingAudio = 0,  // transcribe_file is draining/resampling the stream
        kLoadingModel,       // whisper context is being created from the model
        kTranscribing,       // the decode loop runs; `percent` advances 0..100
        kFinished,           // terminal: Done, failed, or aborted
    };
    std::atomic<int> percent{0};  // 0..100 while kTranscribing
    std::atomic<Stage> stage{Stage::kDecodingAudio};
    std::atomic<bool> cancel{false};  // set by the UI to abort the run
};

struct Options {
    int threads = 4;        // whisper decode threads (<= 0 -> 1)
    int32_t max_len = 0;    // max segment length in characters (0 = none)
    int32_t max_tokens = 0; // max tokens per segment (0 = no limit)
    const char* language = nullptr;  // nullptr / "" / "auto" -> auto-detect
    bool verbose = false;   // whisper progress printing
    Progress* progress = nullptr;  // optional live channel, see above
};

struct Report {
    Result result = Result::kNoModel;
    std::vector<transcript::Segment> cues;  // ms clock, cleaned via transcript law
    std::string error;                       // human-readable detail
    double audio_seconds = 0.0;  // decoded PCM length at 16 kHz (perf readout)
    double wall_seconds = 0.0;   // model load + decode wall time (perf readout)
};

// Resolve the quantized model file. Priority:
//   1. `override` when it names an existing regular file (tests/embedders);
//   2. $CANVAS_WHISPER_MODEL when it names an existing regular file;
//   3. <XDG_CACHE_HOME|~/.cache>/nova-canvas/whisper/ggml-{base,small,medium}.bin
//      in that order (first existing wins).
// Returns "" when nothing resolvable exists.
[[nodiscard]] std::string resolve_model_path(const std::string& override = {});

// PCM: 16 kHz mono float in [-1, 1]. Segments use millisecond times and have
// had clean_segment_text() applied; silent cues are dropped. Thread-safe (the
// context is created and freed inside).
[[nodiscard]] Report transcribe_pcm_16k(const float* pcm, std::size_t n_samples,
                                        const std::string& model_path,
                                        const Options& opts = {});

// Decodes `path`'s full audio stream to 16 kHz mono float (via the shared
// AudioDecoder resample — no local libav code) and transcribes it. Reads the
// whole stream; for background jobs. kBadInput when the file has no decodable
// audio, kNoModel/kCouldNotInit/kRunFailed from the engine as in the PCM law,
// kAborted when Options::progress->cancel was set mid-run.
// Empty cues are normal for a speech-less file (tone, music bed, silence).
[[nodiscard]] Report transcribe_file(const std::string& path,
                                     const std::string& model_path,
                                     const Options& opts = {});

}  // namespace canvas::core::transcribe