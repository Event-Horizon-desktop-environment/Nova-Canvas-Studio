// Unit tests for the whisper.cpp engine seam (canvas::core::transcribe): the
// error-path and model-resolution laws (no model file required), plus a live
// 16 kHz silence transcription that runs only when a real quantized model is
// available via CANVAS_WHISPER_MODEL or the default cache path — that part
// SKIPs otherwise, it never fails the suite.

#include "canvas/core/media/transcribe.hpp"
#include "canvas/core/media/transcript.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

namespace {

int g_failures = 0;

void report(const bool ok, const char* name) {
    std::printf("%s  %s\n", ok ? "PASS" : "FAIL", name);
    if (!ok) ++g_failures;
}

// Empty the env override so resolution tests are deterministic. setenv with ""
// makes getenv return "" for this process only.
void clear_model_env() { ::setenv("CANVAS_WHISPER_MODEL", "", 1); }

// Point the default cache dir at an EMPTY scratch directory so the "-> empty"
// resolution asserts never depend on whether a model currently sits in the
// user's real cache (~/.cache/nova-canvas/whisper). Restored before the live
// block so it can still find the real model.
void push_empty_cache() { ::setenv("XDG_CACHE_HOME", "/tmp/opencode/media/empty_cache", 1); }
void pop_empty_cache() { ::unsetenv("XDG_CACHE_HOME"); }

const char* kGarbage = "/tmp/opencode/media/transcribe_not_a_model.bin";

// Hand-rolled 16-bit PCM mono WAV (0.5 s of 440 Hz stereo sine at 44.1 kHz) so
// transcribe_file has a real decodable audio fixture with no external tools.
bool write_tone_wav(const std::string& path) {
    constexpr uint16_t kCh = 2;
    constexpr uint32_t kSr = 44100;
    constexpr uint32_t kFrames = kSr / 2;
    std::vector<uint8_t> data;
    data.reserve(kFrames * kCh * 2u);
    for (uint32_t i = 0; i < kFrames; ++i) {
        for (uint16_t c = 0; c < kCh; ++c) {
            const int16_t s = static_cast<int16_t>(
                std::sin(2.0 * 3.14159265358979 * 440.0 * i / kSr) * 15000.0);
            data.push_back(static_cast<uint8_t>(s & 0xff));
            data.push_back(static_cast<uint8_t>((s >> 8) & 0xff));
        }
    }
    const uint32_t data_bytes = static_cast<uint32_t>(data.size());
    const uint32_t riff = 36u + data_bytes;
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) return false;
    const char hdr[] = {'R','I','F','F'};
    f.write(hdr, 4);
    f.write(reinterpret_cast<const char*>(&riff), 4);
    const char wave[] = {'W','A','V','E','f','m','t',' '};
    f.write(wave, 8);
    const uint32_t fmt_sz = 16u;
    const uint16_t fmt = 1u;        // PCM
    const uint16_t ch = kCh;
    const uint32_t sr = kSr;
    const uint32_t byte_rate = kSr * kCh * 2u;
    const uint16_t block_align = kCh * 2u;
    const uint16_t bits = 16u;
    f.write(reinterpret_cast<const char*>(&fmt_sz), 4);
    f.write(reinterpret_cast<const char*>(&fmt), 2);
    f.write(reinterpret_cast<const char*>(&ch), 2);
    f.write(reinterpret_cast<const char*>(&sr), 4);
    f.write(reinterpret_cast<const char*>(&byte_rate), 4);
    f.write(reinterpret_cast<const char*>(&block_align), 2);
    f.write(reinterpret_cast<const char*>(&bits), 2);
    const char data_tag[] = {'d','a','t','a'};
    f.write(data_tag, 4);
    f.write(reinterpret_cast<const char*>(&data_bytes), 4);
    f.write(reinterpret_cast<const char*>(data.data()),
            static_cast<std::streamsize>(data_bytes));
    return f.good();
}

}  // namespace

int main() {
    using canvas::core::transcribe::Options;
    using canvas::core::transcribe::Report;
    using canvas::core::transcribe::Result;
    using canvas::core::transcribe::resolve_model_path;
    using canvas::core::transcribe::transcribe_file;
    using canvas::core::transcribe::transcribe_pcm_16k;

    clear_model_env();
    std::remove(kGarbage);  // fixture from a previous run must not exist yet
    push_empty_cache();     // hermetic "-> empty" resolution (see helper)

    // --- model resolution law ------------------------------------------------
    report(resolve_model_path("/nonexistent/model.bin").empty(),
           "resolve: nonexistent override -> empty");
    report(resolve_model_path(kGarbage).empty(),
           "resolve: nonexistent default candidate -> empty");

    // A real scratch file resolves via the override (env is cleared).
    if (std::FILE* f = std::fopen(kGarbage, "wb")) {
        std::fputs("this is not a ggml model", f);
        std::fclose(f);
        report(resolve_model_path(kGarbage) == kGarbage, "resolve: existing override wins");
    } else {
        std::printf("SKIP resolve override test (no /tmp/opencode/media)\n");
    }

    // Env beats the override chain: point CANVAS_WHISPER_MODEL at a bogus path.
    ::setenv("CANVAS_WHISPER_MODEL", "/bogus/env/model.bin", 1);
    report(resolve_model_path().empty(),
           "resolve: unresolvable env path -> empty (env consulted)");
    ::setenv("CANVAS_WHISPER_MODEL", kGarbage, 1);
    report(resolve_model_path("/nonexistent/model.bin") == kGarbage,
           "resolve: env path wins over a bad override");
    clear_model_env();
    pop_empty_cache();

    // --- engine error-path laws (no model needed) ---------------------------
    {
        Report r = transcribe_pcm_16k(nullptr, 0, kGarbage);
        report(r.result == Result::kBadInput, "engine: null pcm -> kBadInput");
    }
    {
        // A validation failure still parks the progress channel at Finished so
        // a polling popup never spins on a stale stage.
        using canvas::core::transcribe::Progress;
        Progress prog;
        Options opt;
        opt.progress = &prog;
        Report r = transcribe_pcm_16k(nullptr, 0, kGarbage, opt);
        report(r.result == Result::kBadInput, "engine: progress run still validates input");
        report(prog.stage.load() == Progress::Stage::kFinished,
               "engine: failed validation parks progress at Finished");
    }
    {
        const float one_sample = 0.0f;
        Report r = transcribe_pcm_16k(&one_sample, 0, kGarbage);
        report(r.result == Result::kBadInput, "engine: zero samples -> kBadInput");
    }
    {
        const float one_sample = 0.0f;
        Report r = transcribe_pcm_16k(&one_sample, 1, "/nonexistent/model.bin");
        report(r.result == Result::kNoModel, "engine: missing model file -> kNoModel");
    }
    {
        // kGarbage exists (written above) but is not a model.
        std::vector<float> silence(16000, 0.0f);
        Report r = transcribe_pcm_16k(silence.data(), silence.size(), kGarbage, Options{});
        report(r.result == Result::kCouldNotInit, "engine: corrupt model file -> kCouldNotInit");
    }

    // --- transcribe_file: decode + downmix + resample + engine --------------
    const std::string tone = "/tmp/opencode/media/transcribe_tone.wav";
    const bool tone_ok = write_tone_wav(tone);
    if (tone_ok) {
        Report r1 = transcribe_file(tone, "/nonexistent/model.bin");
        report(r1.result == Result::kNoModel, "file: missing model -> kNoModel");
        Report r2 = transcribe_file(tone, kGarbage);
        report(r2.result == Result::kCouldNotInit, "file: corrupt model -> kCouldNotInit");
        Report r3 = transcribe_file("/nonexistent/media.wav", kGarbage);
        report(r3.result == Result::kBadInput, "file: missing media -> kBadInput");
    } else {
        std::printf("SKIP transcribe_file tests (could not write fixture wav)\n");
    }

    // --- live engine (runs only when a real model is reachable) -------------
    {
        std::string model = resolve_model_path();
        if (model.empty()) {
            std::printf("SKIP live transcription (no model; set CANVAS_WHISPER_MODEL "
                        "or drop ggml-*.bin under the default cache dir)\n");
        } else {
            std::printf("  using model: %s\n", model.c_str());
            Options opt;
            opt.threads = 4;
            std::vector<float> silence(16000u, 0.0f);  // 1 s of digital silence
            Report r = transcribe_pcm_16k(silence.data(), silence.size(), model, opt);
            report(r.result == Result::kDone, "live: silence transcribes to kDone");
            if (r.result != Result::kDone) std::printf("      err: %s\n", r.error.c_str());
            // Note: whether whisper emits a (possibly empty-text) segment for
            // pure digital silence is the engine's segmentizer policy, not our
            // seam contract — asserting on cue count here would be brittle.

            if (tone_ok) {
                Report rf = transcribe_file(tone, model, opt);
                report(rf.result == Result::kDone, "live: file transcribe reaches kDone");
                if (rf.result == Result::kDone) {
                    const std::string srt_path = "/tmp/opencode/media/transcribe.srt";
                    report(write_srt(srt_path, rf.cues), "live: SRT writer accepts engine cues");
                    std::remove(srt_path.c_str());
                }
                if (rf.result != Result::kDone) std::printf("      err: %s\n", rf.error.c_str());
            }

            // --- progress channel + perf readout -----------------------------
            {
                using canvas::core::transcribe::Progress;
                Progress prog;
                Options popt;
                popt.threads = 4;
                popt.progress = &prog;
                Report rp = transcribe_pcm_16k(silence.data(), silence.size(), model, popt);
                report(rp.result == Result::kDone, "live: progress run reaches kDone");
                report(prog.stage.load() == Progress::Stage::kFinished,
                       "live: progress run parks at Finished");
                report(prog.percent.load() == 100, "live: progress percent ends at 100");
                report(rp.audio_seconds > 0.99 && rp.audio_seconds < 1.01,
                       "live: audio_seconds measures the 1 s fixture");
                report(rp.wall_seconds > 0.0, "live: wall_seconds measures engine time");
            }

            // --- cancel aborts the run ---------------------------------------
            {
                using canvas::core::transcribe::Progress;
                Progress abort_prog;
                abort_prog.cancel.store(true);
                Options aopt;
                aopt.threads = 4;
                aopt.progress = &abort_prog;
                Report ra =
                    transcribe_pcm_16k(silence.data(), silence.size(), model, aopt);
                report(ra.result == Result::kAborted, "live: pre-set cancel aborts the run");
                report(abort_prog.stage.load() == Progress::Stage::kFinished,
                       "live: aborted run parks at Finished");
            }
        }
    }

    const bool ok = g_failures == 0;
    std::printf("%s\n", ok ? "transcribe_test: ALL PASS" : "transcribe_test: FAILURES");
    return ok ? 0 : 1;
}