// Headless AudioPipeline unit tests (Phase 17). Compiles audio_pipeline.cpp
// directly into the binary (like timeline_decoder_test) so the module is
// verified exactly as shipped; the Qt-free seam is enforced by the build.
//
// Uses a synthesized deterministic stereo WAV so playback regions can be
// asserted exactly. Primary regression: a clip trimmed at its HEAD (src_in>0)
// must serve the trim region from the first sample — before the AudioDecoder
// resync-walk fix, a fresh request far forward of the decoder's position served
// the file's beginning (garbled audio at the head of a trimmed clip).

#include "features/playback/audio_pipeline.hpp"
#include "features/playback/audio_sink.hpp"

#include "canvas/core/project/project.hpp"
#include "canvas/core/timeline/audio_fade.hpp"
#include "canvas/core/timeline/model.hpp"

#include "fake_audio_sink.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

using namespace canvas::core;

namespace {

constexpr int kRate = 48000;
constexpr int kChannels = 2;
constexpr double kFps = 30.0;
constexpr int64_t kPerFrame = 1600;        // llround(48k / 30)
constexpr int64_t kPrerollFrames = 3360;   // llround(70ms * 48k)
constexpr int64_t kGrainFrames = 1920;     // llround(40ms * 48k)
constexpr int64_t kScrubChunkFrames = 5760;  // llround(120ms * 48k)

constexpr double kTau = 6.2831853071795865;

bool write_test_wav(const char* path, int seconds = 6) {
    std::FILE* f = std::fopen(path, "wb");
    if (!f) return false;
    const int64_t total = static_cast<int64_t>(kRate) * seconds;
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
    const double start_c[2] = {0.0, 1.7};
    for (int64_t i = 0; i < total; ++i) {
        for (int c = 0; c < kChannels; ++c) {
            const double ph = kTau * (300.0 + 100.0 * c) / kRate * static_cast<double>(i) +
                              start_c[c];
            const float v = static_cast<float>(0.8 * std::sin(ph));
            const std::int16_t s = static_cast<std::int16_t>(std::llround(v * 32767.0f));
            std::fwrite(&s, 2, 1, f);
        }
    }
    std::fclose(f);
    return true;
}

// Ground-truth float for the same waveform (must match write_test_wav).
float exp_sample(int64_t frame, int ch) {
    const double ph = kTau * (300.0 + 100.0 * ch) / kRate * static_cast<double>(frame) +
                      (ch ? 1.7 : 0.0);
    const float v = static_cast<float>(0.8 * std::sin(ph));
    return static_cast<float>(std::llround(v * 32767.0f)) / 32768.0f;
}

// Every float in v[begin..end) (interleaved, absolute source frame `abs_frame`
// at index `begin`) must match the synthesized waveform.
bool region_matches(const std::vector<float>& v, std::size_t begin, std::size_t end,
                    int64_t abs_frame) {
    if (end > v.size()) return false;
    for (std::size_t i = begin; i < end; ++i) {
        const int64_t off = static_cast<int64_t>(i - begin);
        const int64_t fr = abs_frame + off / kChannels;
        const int ch = static_cast<int>(off % kChannels);
        if (std::fabs(v[i] - exp_sample(fr, ch)) > 1e-3f) return false;
    }
    return true;
}

Project make_project(int64_t src_in, int64_t src_out) {
    Project p;
    p.name = "AudioPipelineTest";
    p.sequence.fps = kFps;

    MediaEntry m0;
    m0.id = 0;
    m0.path = "/tmp/canvas_audio_pipe_test.wav";
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
    c.tl_out = src_out - src_in;
    c.src_in = src_in;
    c.src_out = src_out;
    c.enabled = true;
    a1.clips.push_back(c);
    p.sequence.audio_tracks.push_back(std::move(a1));
    return p;
}

struct Harness {
    canvas::gui::test::FakeAudioSink sink;
    canvas::gui::AudioPipeline pipe{sink};
    Project project{make_project(0, 180)};

    Harness(int64_t src_in = 0, int64_t src_out = 180) : project(make_project(src_in, src_out)) {
        pipe.set_project(&project);
        pipe.add_media(project.media[0]);
        pipe.open_output();
    }

    void play(int first_seq, int count, bool seek_hold = false) {
        for (int k = 0; k < count; ++k)
            pipe.play_step(first_seq + k, 1.0 / kFps, seek_hold);
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
    if (std::getenv("CANVAS_PLAYBACK_DEBUG")) check(true, "notice: playback debug env set (accepted)");
    const char* wav = "/tmp/canvas_audio_pipe_test.wav";
    check(write_test_wav(wav), "write test wav");
    if (!write_test_wav(wav)) return 1;

    // --- A. Untrimmed: preroll + steady play is one contiguous region ---------
    {
        Harness h;
        check(h.pipe.is_active(), "A: output open");
        check(h.sink.rate == kRate && h.sink.channels == kChannels, "A: device opened at 48k/stereo");
        h.pipe.rewind(0, true);
        h.pipe.preroll(0, 70, true);
        check(h.sink.written_total_ == static_cast<uint64_t>(kPrerollFrames),
              "A: preroll writes exactly the lead");
        h.play(0, 10);
        const uint64_t total = h.sink.written_total_;
        check(total == static_cast<uint64_t>(3360 + 1440 + 1600 * 7),
              "A: preroll(3.36k)+catchup(1.44k)+steady(7*1.6k) == 16k frames");
        check(region_matches(h.sink.all_, 0, static_cast<std::size_t>(total) * 2, 0),
              "A: entire received stream equals source samples [0,16k)");
        check(h.sink.pending_frames() == static_cast<std::size_t>(total),
              "A: unfushed device still holds all 16k frames pending");
    }

    // --- B. rewind re-anchors: a second run rewrites the same region ----------
    {
        Harness h;
        h.pipe.rewind(0, true);
        h.pipe.preroll(0, 70, true);
        h.play(0, 10);
        const std::size_t run1_begin = 0, run1_end = h.sink.all_.size();
        h.pipe.rewind(0, true);
        h.pipe.preroll(0, 70, true);
        h.play(0, 10);
        const std::size_t run2_begin = run1_end, run2_end = h.sink.all_.size();
        check(run2_end == run2_begin + (run1_end - run1_begin),
              "B: second run writes the same frame count as the first");
        check(region_matches(h.sink.all_, run1_begin, run1_end, 0),
              "B: first run is source samples [0,16k)");
        check(region_matches(h.sink.all_, run2_begin, run2_end, 0),
              "B: after rewind the run re-anchored and rewrote [0,16k) (no stale audio)");
        check(h.pipe.feed_watermark_active(), "B: feed watermark active after preroll within a clip");
    }

    // --- C. TRIMMED HEAD regression: fresh decoder serves the trim region ------
    {
        Harness h(/*src_in=*/32, /*src_out=*/180);
        h.pipe.rewind(0, true);
        h.pipe.preroll(0, 70, true);
        h.play(0, 10);
        const uint64_t total = h.sink.written_total_;
        check(total == static_cast<uint64_t>(16000),
              "C: trimmed run writes 16k frames (3360+1440+7*1600)");
        const int64_t head_sample = 32 * kPerFrame;  // 51200
        check(region_matches(h.sink.all_, 0, static_cast<std::size_t>(total) * 2, head_sample),
              "C: trimmed clip serves the TRIM region [51200,67200), not the file start");
    }

    // --- D. audible scrub grain + media queries ---------------------------------
    {
        Harness h(32, 180);
        h.pipe.rewind(10, false);
        h.pipe.play_scrub_grain(10);
        const uint64_t total = h.sink.written_total_;
        check(total == static_cast<uint64_t>(kGrainFrames), "D: grain is a 40ms chunk (1920 frames)");
        check(region_matches(h.sink.all_, 0, static_cast<std::size_t>(total) * 2, 42 * kPerFrame),
              "D: grain decodes the pos-10 trim region [67200,69120)");
        check(h.pipe.audio_media_at(70) == 0, "D: audio clip (media 0) covers seq 70");
        check(h.pipe.audio_media_at(148) == -1, "D: no audio clip past the clip end (seq 148)");
    }

    // --- E. audible_seq_frame AnMa exact math (latency-free sink) --------------
    {
        Harness h(32, 180);
        h.pipe.rewind(30, true);   // anchor = (32+30)*1600 = 99200
        h.play(60, 3);             // 3 frames, 4800 frames [147200,152000)
        check(h.pipe.audible_seq_frame(70) == 33,
              "E: audible_seq_frame maps audible sample 104000 back to seq 33");
        check(h.pipe.audible_seq_frame(148) == -1,
              "E: nothing audible past the clip end -> -1");
    }

    // --- F. feed_scrub_audio: reposition + throttle -----------------------------
    {
        Harness h(32, 180);
        h.pipe.begin_scrub();
        h.pipe.feed_scrub_audio(40);
        check(h.sink.pending_frames() == static_cast<std::size_t>(kScrubChunkFrames),
              "F: scrub feed 120ms chunk pending (5760 frames)");
        check(region_matches(h.sink.queue_, 0, kScrubChunkFrames * 2, 72 * kPerFrame),
              "F: scrub chunk decodes pos-40 trim region [115200,120960)");
        check(h.sink.all_.empty(),
              "F: reposition fed the device queue only (cumulative history untouched)");
        check(h.pipe.repositions_since_begin() == 1, "F: first feed countss one reposition");
        h.pipe.feed_scrub_audio(41);  // within the 45ms throttle window -> no-op
        check(h.pipe.repositions_since_begin() == 1,
              "F: rapid second feed is throttled (no extra reposition)");
        check(h.sink.pending_frames() == static_cast<std::size_t>(kScrubChunkFrames),
              "F: throttled feed did not replace the pending chunk");
    }

    // --- G. AUDIO TRANSITION FADE is AUDIBLE in playback -----------------------
    // A clip with an OUT fade must deliver attenuated samples to the output
    // device inside the fade window (unity before it). This is what makes the
    // linked-pair feature real: the translated AudioFade* type on the audio mate
    // changes what you HEAR when playing the timeline.
    {
        Harness h;  // clip tl 0..180, src 0..180
        Clip& clip = h.project.sequence.audio_tracks[0].clips[0];
        clip.transition_out = TransitionType::AudioFadeConstantGain;
        clip.transition_out_duration = 10;  // window = tl frames [170,180)
        h.pipe.rewind(166, true);
        h.play(166, 14);  // media samples [265600, 288000), 14*1600 frames
        const uint64_t total = h.sink.written_total_;
        check(total == 14u * kPerFrame, "G: fade window run writes 14 frames (22.4k samples)");
        bool ok = total == 14u * kPerFrame;
        const int64_t base = 166 * kPerFrame;
        for (std::size_t s = 0; s < h.sink.all_.size() && ok; ++s) {
            const int64_t media = base + static_cast<int64_t>(s / kChannels);
            const int64_t tl = media / kPerFrame;
            const float g = audio_fade_gain(clip, tl);
            const float want = exp_sample(media, static_cast<int>(s % kChannels)) * g;
            if (std::fabs(h.sink.all_[s] - want) > 1e-3f) ok = false;
        }
        check(ok, "G: fade envelope scales playback samples inside the transition window");
        check(region_matches(h.sink.all_, 0, kPerFrame * 2u, 166 * kPerFrame),
              "G: audio before the fade window is at unity gain");
    }

    // --- H. lifecycle ------------------------------------------------------------
    {
        canvas::gui::test::FakeAudioSink sink;
        canvas::gui::AudioPipeline pipe(sink);
        check(!pipe.is_active(), "G: inactive before open_output");
        pipe.open_output();
        check(pipe.is_active(), "G: active after open_output");
        check(sink.is_open(), "G: sink open");
        pipe.close_output();
        check(!pipe.is_active() && !sink.is_open(), "G: close_output deactivates and closes");
        pipe.reset();
        check(!pipe.is_active(), "G: reset leaves the pipeline inactive");
    }

    std::remove(wav);

    if (failures) {
        std::fprintf(stderr, "audio_pipeline_test: %d failure(s)\n", failures);
        return 1;
    }
    std::printf("audio_pipeline_test: all passed\n");
    return 0;
}