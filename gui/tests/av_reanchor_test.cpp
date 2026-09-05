// Headless integration test (Phase 19): SonicSync + AudioPipeline together,
// encoding the MLT "audio rides with its frame" invariant.
//
// The scenario this guards: a seek-while-playing. The worker begins decoding the
// (slow) target frame with the picture frozen; SonicSync engages a hold that
// must gate OFF the per-frame audio feed, else new-position audio would stream
// ahead of the frozen picture. Only after the target frame is presented
// (end_seek_hold) may the feed resume, re-anchoring audio to its frame.
//
// Drives the in-memory FakeAudioSink so no sound device is needed. Compiles
// sonicsync.cpp + audio_pipeline.cpp directly (Qt-free seam enforced by build).

#include "features/playback/audio_pipeline.hpp"
#include "features/playback/audio_sink.hpp"
#include "features/playback/sonicsync.hpp"

#include "canvas/core/project/project.hpp"
#include "canvas/core/timeline/audio_fade.hpp"
#include "canvas/core/timeline/model.hpp"

#include "fake_audio_sink.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <vector>

using namespace canvas::core;

namespace {

constexpr int kRate = 48000;
constexpr int kChannels = 2;
constexpr double kFps = 30.0;
constexpr int kPerFrame = 1600;  // llround(48k / 30)
constexpr int64_t kPrerollFrames = 3360;

constexpr double kTau = 6.2831853071795865;

bool write_test_wav(const char* path) {
    std::FILE* f = std::fopen(path, "wb");
    if (!f) return false;
    const int64_t total = static_cast<int64_t>(kRate) * 6;
    const std::uint32_t data_bytes = static_cast<std::uint32_t>(total) * kChannels * 2;
    const std::uint32_t byte_rate = kRate * (kChannels * 2);
    const std::uint16_t block_align = static_cast<std::uint16_t>(kChannels * 2);
    const std::uint32_t riff = data_bytes + 36;
    const std::uint32_t fmt_sz = 16;
    const std::uint16_t fmt1 = 1;
    const std::uint16_t bps = 16;
    std::fwrite("RIFF", 1, 4, f);
    std::fwrite(&riff, 4, 1, f);
    std::fwrite("WAVE", 1, 4, f);
    std::fwrite("fmt ", 1, 4, f);
    std::fwrite(&fmt_sz, 4, 1, f);
    std::fwrite(&fmt1, 2, 1, f);
    std::fwrite(&kChannels, 2, 1, f);
    std::fwrite(&kRate, 4, 1, f);
    std::fwrite(&byte_rate, 4, 1, f);
    std::fwrite(&block_align, 2, 1, f);
    std::fwrite(&bps, 2, 1, f);
    std::fwrite("data", 1, 4, f);
    std::fwrite(&data_bytes, 4, 1, f);
    for (int64_t i = 0; i < total; ++i)
        for (int c = 0; c < kChannels; ++c) {
            const double ph = kTau * (300.0 + 100.0 * c) / kRate * static_cast<double>(i);
            const float v = static_cast<float>(0.8 * std::sin(ph));
            const std::int16_t s = static_cast<std::int16_t>(std::llround(v * 32767.0f));
            std::fwrite(&s, 2, 1, f);
        }
    std::fclose(f);
    return true;
}

Project make_project() {
    Project p;
    p.name = "AvReanchorTest";
    p.sequence.fps = kFps;

    MediaEntry m0;
    m0.id = 0;
    m0.path = "/tmp/canvas_av_reanchor_test.wav";
    m0.fps = kFps;
    m0.width = 320;
    m0.height = 180;
    m0.total_frames = 180;
    m0.bin = "Scratch";
    p.media.push_back(m0);
    p.bins.push_back("Scratch");

    Track a1;
    a1.kind = Track::Kind::Audio;
    a1.name = "A1";
    Clip c;
    c.media = 0;
    c.name = "Tone";
    c.tl_in = 0;
    c.tl_out = 180;
    c.src_in = 0;
    c.src_out = 180;
    c.enabled = true;
    a1.clips.push_back(c);
    p.sequence.audio_tracks.push_back(std::move(a1));
    return p;
}

struct Harness {
    canvas::gui::test::FakeAudioSink sink;
    canvas::gui::AudioPipeline pipe{sink};
    canvas::gui::SonicSync sync;
    Project project{make_project()};

    Harness() {
        pipe.set_project(&project);
        pipe.add_media(project.media[0]);
        pipe.open_output();
        pipe.rewind(0, true);
        pipe.preroll(0, 70, true);
    }
};

int failures = 0;

void check(bool cond, const char* what) {
    if (!cond) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++failures;
    } else {
        std::printf("ok:   %s\n", what);
    }
}

}  // namespace

int main() {
    const char* wav = "/tmp/canvas_av_reanchor_test.wav";
    if (!write_test_wav(wav)) {
        std::fprintf(stderr, "FAIL: could not write wav\n");
        return 1;
    }

    // --- Steady feed baseline: audio flows while playing ---
    Harness h;
    const uint64_t baseline = h.sink.written_total_;
    check(baseline == static_cast<uint64_t>(kPrerollFrames), "baseline: preroll wrote the lead");
    // seq 10 is well past the ~70ms(≈seq 2) preroll window, so the feed must
    // actually write audio there rather than being suppressed by the watermark.
    h.pipe.play_step(10, 1.0 / kFps, false);  // seek_hold INACTIVE
    const uint64_t after_steady = h.sink.written_total_;
    check(after_steady > baseline, "steady: feed advances while playing (hold inactive)");

    // --- Seek while playing: held feed must NOT advance ---
    h.pipe.rewind(30, true);  // re-anchor a fresh play run at seq 30
    h.pipe.preroll(30, 70, true);
    const uint64_t seek_anchor = h.sink.written_total_;

    // Begin the seek-hold for the target the worker is about to decode.
    h.sync.begin_seek_hold(90);
    check(h.sync.seek_hold_active(), "seek: hold engages on begin_seek_hold");
    check(h.sync.pending_reanchor(), "seek: re-anchor flagged");

    // While the hold is active the feed is suppressed, even if called.
    const uint64_t before = h.sink.written_total_;
    h.pipe.play_step(90, 1.0 / kFps, h.sync.seek_hold_active());
    h.pipe.play_step(91, 1.0 / kFps, h.sync.seek_hold_active());
    check(h.sink.written_total_ == before, "seek: per-frame feed suppressed while hold active");

    // Presenting the target frame releases the hold and clears the re-anchor.
    h.sync.end_seek_hold();
    check(!h.sync.seek_hold_active(), "seek: hold released on end_seek_hold");
    check(!h.sync.pending_reanchor(), "seek: re-anchor cleared on release");

    // Feed resumes now that audio may ride with its (presented) frame.
    const uint64_t resume_anchor = h.sink.written_total_;
    h.pipe.play_step(90, 1.0 / kFps, h.sync.seek_hold_active());
    check(h.sink.written_total_ > resume_anchor,
          "seek: feed resumes after release (audio re-anchored to its frame)");

    // --- Paused seek: re-anchor flagged but no feed suppression ---
    Harness h2;
    h2.sync.on_seek_paused(120);
    check(!h2.sync.seek_hold_active(), "paused: no hold (audio irrelevant while paused)");
    check(h2.sync.pending_reanchor(), "paused: re-anchor flagged for next play");
    const uint64_t pause_anchor = h2.sink.written_total_;
    h2.pipe.play_step(5, 1.0 / kFps, h2.sync.seek_hold_active());
    check(h2.sink.written_total_ > pause_anchor,
          "paused->play: feed flows (no hold in paused path)");

    if (failures == 0) {
        std::printf("av_reanchor_test: ALL PASS\n");
        return 0;
    }
    std::printf("av_reanchor_test: %d FAILURE(S)\n", failures);
    return 1;
}