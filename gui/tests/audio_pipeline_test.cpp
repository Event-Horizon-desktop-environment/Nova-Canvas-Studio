#include "features/playback/audio_pipeline.hpp"
#include "features/playback/audio_sink.hpp"
#include "features/playback/sync_constants.hpp"
#include "canvas/core/project/project.hpp"
#include "canvas/core/timeline/audio_fade.hpp"
#include "canvas/core/timeline/audio_mix.hpp"
#include "canvas/core/timeline/model.hpp"

#include "fake_audio_sink.hpp"

#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

using namespace canvas::core;

namespace {

constexpr int kRate = 48000;
constexpr int kChannels = 2;
constexpr double kFps = 30.0;
constexpr int64_t kPerFrame = 1600;
constexpr int64_t kPrerollFrames = 3360;
constexpr int64_t kGrainFrames = 1920;
constexpr int64_t kScrubChunkFrames = 5760;

constexpr double kTau = 6.2831853071795865;

bool write_wav_channels(const char* path, int seconds, double base_freq, float amp,
                        int channels, const double* phases) {
    std::FILE* f = std::fopen(path, "wb");
    if (!f || channels <= 0) {
        if (f) std::fclose(f);
        return false;
    }
    const int64_t total = static_cast<int64_t>(kRate) * seconds;
    const std::uint32_t data_bytes = static_cast<std::uint32_t>(total) * channels * 2;
    const std::uint32_t byte_rate = kRate * (channels * 2);
    const std::uint16_t block_align = static_cast<std::uint16_t>(channels * 2);
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
    std::fwrite(&channels, 2, 1, f);
    std::fwrite(&kRate, 4, 1, f);
    std::fwrite(&byte_rate, 4, 1, f);
    std::fwrite(&block_align, 2, 1, f);
    std::fwrite(&bps, 2, 1, f);
    std::fwrite("data", 1, 4, f);
    std::fwrite(&data_bytes, 4, 1, f);
    for (int64_t i = 0; i < total; ++i) {
        for (int c = 0; c < channels; ++c) {
            const double ph = kTau * (base_freq + 100.0 * c) / kRate *
                                  static_cast<double>(i) +
                              phases[c];
            const float v = static_cast<float>(amp * std::sin(ph));
            const std::int16_t s = static_cast<std::int16_t>(std::llround(v * 32767.0f));
            std::fwrite(&s, 2, 1, f);
        }
    }
    std::fclose(f);
    return true;
}

float exp_wave_ch(int64_t frame, int ch, double base_freq, float amp,
                  const double* phases) {
    const double ph = kTau * (base_freq + 100.0 * ch) / kRate *
                          static_cast<double>(frame) +
                      phases[ch];
    const float v = static_cast<float>(amp * std::sin(ph));
    return static_cast<float>(std::llround(v * 32767.0f)) / 32768.0f;
}

bool write_wav(const char* path, int seconds, double base_freq, float amp,
               double start_l, double start_r) {
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
    for (int64_t i = 0; i < total; ++i) {
        for (int c = 0; c < kChannels; ++c) {
            const double ph = kTau * (base_freq + 100.0 * c) / kRate *
                                  static_cast<double>(i) +
                              (c ? start_r : start_l);
            const float v = static_cast<float>(amp * std::sin(ph));
            const std::int16_t s = static_cast<std::int16_t>(std::llround(v * 32767.0f));
            std::fwrite(&s, 2, 1, f);
        }
    }
    std::fclose(f);
    return true;
}

bool write_test_wav(const char* path, int seconds = 6) {
    return write_wav(path, seconds, 300.0, 0.8f, 0.0, 1.7);
}

float exp_wave(int64_t frame, int ch, double base_freq, float amp,
               double start_l, double start_r) {
    const double ph = kTau * (base_freq + 100.0 * ch) / kRate *
                          static_cast<double>(frame) +
                      (ch ? start_r : start_l);
    const float v = static_cast<float>(amp * std::sin(ph));
    return static_cast<float>(std::llround(v * 32767.0f)) / 32768.0f;
}

float exp_sample(int64_t frame, int ch) {
    return exp_wave(frame, ch, 300.0, 0.8f, 0.0, 1.7);
}

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
    c.id = p.sequence.next_clip_id++;
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

}

int main() {
    if (std::getenv("CANVAS_PLAYBACK_DEBUG")) check(true, "notice: playback debug env set (accepted)");
    const char* wav = "/tmp/canvas_audio_pipe_test.wav";
    check(write_test_wav(wav), "write test wav");
    if (!write_test_wav(wav)) return 1;

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
        check(total == static_cast<uint64_t>(3360 + 1440 + 1600 * 8),
              "A: preroll(3.36k)+catchup(1.44k)+steady(8*1.6k) == 17.6k frames");
        check(region_matches(h.sink.all_, 0, static_cast<std::size_t>(total) * 2, 0),
              "A: entire received stream equals source samples [0,17.6k)");
        check(h.sink.pending_frames() == static_cast<std::size_t>(total),
              "A: unfushed device still holds all 17.6k frames pending");
    }

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

    {
        Harness h(32, 180);
        h.pipe.rewind(0, true);
        h.pipe.preroll(0, 70, true);
        h.play(0, 10);
        const uint64_t total = h.sink.written_total_;
        check(total == static_cast<uint64_t>(3360 + 1440 + 1600 * 8),
              "C: trimmed run writes 17.6k frames (3360+1440+8*1600)");
        const int64_t head_sample = 32 * kPerFrame;
        check(region_matches(h.sink.all_, 0, static_cast<std::size_t>(total) * 2, head_sample),
              "C: trimmed clip serves the TRIM region [51200,68800), not the file start");
    }

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

    {
        Harness h(32, 180);
        h.pipe.rewind(30, true);
        h.play(60, 3);
        check(h.pipe.audible_seq_frame(70) == 33,
              "E: audible_seq_frame maps audible sample 104000 (= anchor 99200 + 4800) back to seq 33");
        check(h.pipe.audible_seq_frame(148) == -1,
              "E: nothing audible past the clip end -> -1");
    }

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
        h.pipe.feed_scrub_audio(41);
        check(h.pipe.repositions_since_begin() == 1,
              "F: rapid second feed is throttled (no extra reposition)");
        check(h.sink.pending_frames() == static_cast<std::size_t>(kScrubChunkFrames),
              "F: throttled feed did not replace the pending chunk");
    }

    {
        Harness h;
        Clip& clip = h.project.sequence.audio_tracks[0].clips[0];
        clip.transition_out = TransitionType::AudioFadeConstantGain;
        clip.transition_out_duration = 10;
        h.pipe.rewind(166, true);
        h.play(166, 14);
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

    {
        const char* wav2 = "/tmp/canvas_audio_pipe_test2.wav";
        check(write_wav(wav2, 6, 300.0, 0.5f, 0.9, 0.2), "G2: write second tone wav");

        Project p = make_project(0, 180);
        p.name = "MixTest";
        MediaEntry m1;
        m1.id = 1;
        m1.path = wav2;
        m1.fps = kFps;
        m1.width = 320;
        m1.height = 180;
        m1.total_frames = 180;
        m1.bin = "Scratch";
        p.media.push_back(m1);
        Track a2;
        a2.kind = Track::Kind::Audio;
        a2.name = "A2";
        Clip c2;
        c2.id = p.sequence.next_clip_id++;
        c2.media = 1;
        c2.name = "Tone2";
        c2.tl_in = 0;
        c2.tl_out = 180;
        c2.src_in = 0;
        c2.src_out = 180;
        c2.enabled = true;
        a2.clips.push_back(c2);
        p.sequence.audio_tracks.push_back(std::move(a2));

        const auto run = [&]() -> std::vector<float> {
            canvas::gui::test::FakeAudioSink sink;
            canvas::gui::AudioPipeline pipe{sink};
            pipe.set_project(&p);
            pipe.add_media(p.media[0]);
            pipe.add_media(p.media[1]);
            pipe.open_output();
            pipe.rewind(0, true);
            for (int k = 0; k < 10; ++k) pipe.play_step(k, 1.0 / kFps, false);
            return sink.all_;
        };

        const float g6 = canvas::core::audio_mix::db_to_gain(-6.0f);

        {
            const auto all = run();
            check(all.size() == 16000u * 2, "G2: two-track run writes the same frame count");
            bool ok = all.size() == 16000u * 2;
            for (std::size_t i = 0; i < all.size() && ok; ++i) {
                const float want =
                    exp_sample(static_cast<int64_t>(i / kChannels),
                               static_cast<int>(i % kChannels)) +
                    exp_wave(static_cast<int64_t>(i / kChannels),
                             static_cast<int>(i % kChannels), 300.0, 0.5f, 0.9, 0.2);
                if (std::fabs(all[i] - want) > 1e-3f) ok = false;
            }
            check(ok, "G2: two audio tracks SUM at unity");
        }

        {
            p.sequence.audio_tracks[0].muted = true;
            const auto all = run();
            bool ok = !all.empty();
            for (std::size_t i = 0; i < all.size() && ok; ++i) {
                const float want = exp_wave(static_cast<int64_t>(i / kChannels),
                                            static_cast<int>(i % kChannels), 300.0, 0.5f, 0.9, 0.2);
                if (std::fabs(all[i] - want) > 1e-3f) ok = false;
            }
            check(ok, "G2: muted audio track is silent (A2 alone)");
            p.sequence.audio_tracks[0].muted = false;
        }

        {
            p.sequence.audio_tracks[1].solo = true;
            const auto all = run();
            bool ok = !all.empty();
            for (std::size_t i = 0; i < all.size() && ok; ++i) {
                const float want = exp_wave(static_cast<int64_t>(i / kChannels),
                                            static_cast<int>(i % kChannels), 300.0, 0.5f, 0.9, 0.2);
                if (std::fabs(all[i] - want) > 1e-3f) ok = false;
            }
            check(ok, "G2: solo isolates the soloed track (A1 silenced)");
            p.sequence.audio_tracks[1].solo = false;
        }

        {
            p.sequence.audio_tracks[1].clips[0].pan = 1.0f;
            const auto all = run();
            bool ok = !all.empty();
            for (std::size_t i = 0; i < all.size() && ok; ++i) {
                const int64_t fr = static_cast<int64_t>(i / kChannels);
                const int ch = static_cast<int>(i % kChannels);
                const float s2 = exp_wave(fr, ch, 300.0, 0.5f, 0.9, 0.2);
                const float want = exp_sample(fr, ch) + (ch == 0 ? 0.0f : s2);
                if (std::fabs(all[i] - want) > 1e-3f) ok = false;
            }
            check(ok, "G2: hard-panned track is silent on the opposite channel");
            p.sequence.audio_tracks[1].clips[0].pan = 0.0f;
        }

        {
            p.sequence.audio_tracks[1].clips[0].volume_db = -6.0f;
            const auto all = run();
            bool ok = !all.empty();
            for (std::size_t i = 0; i < all.size() && ok; ++i) {
                const int64_t fr = static_cast<int64_t>(i / kChannels);
                const int ch = static_cast<int>(i % kChannels);
                const float want =
                    exp_sample(fr, ch) +
                    exp_wave(fr, ch, 300.0, 0.5f, 0.9, 0.2) * g6;
                if (std::fabs(all[i] - want) > 1.5e-3f) ok = false;
            }
            check(ok, "G2: -6 dB clip volume halves the track's contribution");
            p.sequence.audio_tracks[1].clips[0].volume_db = 0.0f;
        }

        {
            p.sequence.audio_tracks[0].gain_db = -6.0f;
            const auto all = run();
            bool ok = !all.empty();
            for (std::size_t i = 0; i < all.size() && ok; ++i) {
                const int64_t fr = static_cast<int64_t>(i / kChannels);
                const int ch = static_cast<int>(i % kChannels);
                const float want =
                    exp_sample(fr, ch) * g6 +
                    exp_wave(fr, ch, 300.0, 0.5f, 0.9, 0.2);
                if (std::fabs(all[i] - want) > 1.5e-3f) ok = false;
            }
            check(ok, "G3: -6 dB track gain scales only that audio track");
            p.sequence.audio_tracks[0].gain_db = 0.0f;
        }

        {
            const auto all = run();
            bool ok = !all.empty();
            for (std::size_t i = 0; i < all.size() && ok; ++i) {
                const int64_t fr = static_cast<int64_t>(i / kChannels);
                const int ch = static_cast<int>(i % kChannels);
                const float want =
                    exp_sample(fr, ch) +
                    exp_wave(fr, ch, 300.0, 0.5f, 0.9, 0.2);
                if (std::fabs(all[i] - want) > 1e-3f) ok = false;
            }
            check(ok, "G3: 0 dB track gain is a bit-exact sum (unity shortcut)");
        }

        std::remove(wav2);
    }

    {
        const char* wav4 = "/tmp/canvas_audio_pipe_ch4.wav";
        const double ph4[4] = {0.0, 0.5, 1.1, 2.2};
        check(write_wav_channels(wav4, 6, 300.0, 0.5f, 4, ph4), "G4: write 4ch wav");

        Project p4 = make_project(0, 180);
        MediaEntry m4;
        m4.id = 10;
        m4.path = wav4;
        m4.fps = kFps;
        m4.width = 320; m4.height = 180; m4.total_frames = 180; m4.bin = "Scratch";
        p4.media.push_back(m4);
        p4.sequence.audio_tracks[0].clips[0].media = 10;
        p4.sequence.audio_tracks[0].clips[0].name = "Quad";

        canvas::gui::test::FakeAudioSink sink4;
        canvas::gui::AudioPipeline pipe4{sink4};
        pipe4.set_channels(4);
        pipe4.set_project(&p4);
        pipe4.add_media(p4.media[1]);
        pipe4.open_output();
        check(pipe4.channels() == 4 && sink4.channels == 4, "G4: device opened at 4ch");
        pipe4.rewind(0, true);
        for (int k = 0; k < 5; ++k) pipe4.play_step(k, 1.0 / kFps, false);
        bool ok4 = sink4.channels == 4 && !sink4.all_.empty();
        for (std::size_t i = 0; i < sink4.all_.size() && ok4; ++i) {
            const int64_t fr = static_cast<int64_t>(i / 4);
            const int ch = static_cast<int>(i % 4);
            const float want = exp_wave_ch(fr, ch, 300.0, 0.5f, ph4);
            if (std::fabs(sink4.all_[i] - want) > 1e-3f) ok4 = false;
        }
        check(ok4, "G4: 4ch source maps channel-for-channel on a 4ch device");

        Project pst = make_project(0, 180);
        canvas::gui::test::FakeAudioSink sinkst;
        canvas::gui::AudioPipeline pipest{sinkst};
        pipest.set_channels(4);
        pipest.set_project(&pst);
        pipest.add_media(pst.media[0]);
        pipest.open_output();
        pipest.rewind(0, true);
        for (int k = 0; k < 5; ++k) pipest.play_step(k, 1.0 / kFps, false);
        bool okst = !sinkst.all_.empty();
        for (std::size_t i = 0; i < sinkst.all_.size() && okst; ++i) {
            const int64_t fr = static_cast<int64_t>(i / 4);
            const int ch = static_cast<int>(i % 4);
            const float want = ch < 2 ? exp_sample(fr, ch) : 0.0f;
            if (std::fabs(sinkst.all_[i] - want) > 1e-3f) okst = false;
        }
        check(okst, "G4: stereo source occupies the front pair on a 4ch bus");

        const char* wavm = "/tmp/canvas_audio_pipe_mono.wav";
        const double phm[1] = {0.0};
        check(write_wav_channels(wavm, 6, 250.0, 0.6f, 1, phm), "G4: write mono wav");
        Project pm = make_project(0, 180);
        MediaEntry mm;
        mm.id = 11;
        mm.path = wavm;
        mm.fps = kFps;
        mm.width = 320; mm.height = 180; mm.total_frames = 180; mm.bin = "Scratch";
        pm.media.push_back(mm);
        pm.sequence.audio_tracks[0].clips[0].media = 11;
        canvas::gui::test::FakeAudioSink sinkm;
        canvas::gui::AudioPipeline pipem{sinkm};
        pipem.set_channels(4);
        pipem.set_project(&pm);
        pipem.add_media(pm.media[1]);
        pipem.open_output();
        pipem.rewind(0, true);
        for (int k = 0; k < 5; ++k) pipem.play_step(k, 1.0 / kFps, false);
        bool okm = !sinkm.all_.empty();
        for (std::size_t i = 0; i < sinkm.all_.size() && okm; ++i) {
            const int64_t fr = static_cast<int64_t>(i / 4);
            const int ch = static_cast<int>(i % 4);
            const float mono = exp_wave_ch(fr, 0, 250.0, 0.6f, phm);
            const float want = ch < 2 ? mono : 0.0f;
            if (std::fabs(sinkm.all_[i] - want) > 1e-3f) okm = false;
        }
        check(okm, "G4: mono source broadcasts to the front pair (surrounds silent)");

        std::remove(wav4);
        std::remove(wavm);
    }

    {
        Project p = make_project(0, 180);
        canvas::gui::test::FakeAudioSink sink;
        canvas::gui::AudioPipeline pipe{sink};
        pipe.set_project(&p);
        pipe.add_media(p.media[0]);
        pipe.open_output();
        pipe.rewind(0, true);
        const std::size_t lead = static_cast<std::size_t>(
            static_cast<double>(kRate) * canvas::gui::kAudioLeadMs / 1000.0);
        const std::size_t cap = 2 * static_cast<std::size_t>(kPerFrame);
        std::size_t prev = 0;
        int steps = 0;
        int max_delta = 0;
        while (sink.pending_frames() < lead && steps < 24) {
            pipe.play_step(steps, 1.0 / kFps, false);
            const std::size_t after = sink.pending_frames();
            const int delta = static_cast<int>(after - prev);
            if (delta > max_delta) max_delta = delta;
            prev = after;
            ++steps;
        }
        check(sink.pending_frames() >= lead, "K: live lead buffer reaches kAudioLeadMs");
        check(steps <= 8, "K: lead covered within a few steps (bounded catch-up)");
        check(static_cast<std::size_t>(max_delta) <= cap,
              "K: no single step writes more than two step-budgets (no giant pre-fill)");
        int steady_delta = 0;
        for (int k = 0; k < 60; ++k) {
            pipe.play_step(60 + k, 1.0 / kFps, false);
            const int delta = static_cast<int>(sink.pending_frames() - prev);
            if (delta > steady_delta) steady_delta = delta;
            prev = sink.pending_frames();
        }
        check(static_cast<std::size_t>(steady_delta) <= static_cast<std::size_t>(kPerFrame),
              "K: once covered, each step feeds at most one step-budget (no runaway refill)");
    }

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

    {
        const char* wavH2 = "/tmp/canvas_audio_pipe_mix2.wav";
        check(write_wav(wavH2, 6, 300.0, 0.5f, 0.9, 0.2), "H: write second mix wav");

        Project p = make_project(0, 180);
        MediaEntry m1;
        m1.id = 1;
        m1.path = wavH2;
        m1.fps = kFps;
        m1.width = 320;
        m1.height = 180;
        m1.total_frames = 180;
        m1.bin = "Scratch";
        p.media.push_back(m1);
        Track a2;
        a2.kind = Track::Kind::Audio;
        a2.name = "A2";
        Clip c2;
        c2.id = p.sequence.next_clip_id++;
        c2.media = 1;
        c2.name = "Tone2";
        c2.tl_in = 0;
        c2.tl_out = 180;
        c2.src_in = 0;
        c2.src_out = 180;
        c2.enabled = true;
        a2.clips.push_back(c2);
        p.sequence.audio_tracks.push_back(std::move(a2));

        canvas::gui::test::FakeAudioSink sink;
        canvas::gui::AudioPipeline pipe{sink};
        pipe.set_project(&p);
        pipe.add_media(p.media[0]);
        pipe.add_media(p.media[1]);
        pipe.open_output();
        pipe.rewind(0, true);

        const auto before = sink.written_total_;
        bool all_wrote = true;
        for (int k = 0; k < 180; ++k) {
            pipe.play_step(k, 1.0 / kFps, false);
            const uint64_t now = sink.written_total_;
            if (now == before) { all_wrote = false; break; }
        }
        check(all_wrote, "H: every play_step in a 2-track run writes audio (no silent step)");
        const auto lead = static_cast<uint64_t>(
            static_cast<double>(kRate) * canvas::gui::kAudioLeadMs / 1000.0);
        check(sink.written_total_ >= before + 180u * kPerFrame,
              "H: steady 2-track play wrote at least a step's worth per frame");
        check(sink.written_total_ <= before + 180u * kPerFrame + lead + kPerFrame,
              "H: any pre-fill beyond the steps is bounded by the kAudioLeadMs lead");

        std::remove(wavH2);
    }

    {
        Project p = make_project(0, 180);
        Clip& k1 = p.sequence.audio_tracks[0].clips[0];
        k1.tl_out = 90;
        k1.src_out = 90;
        Clip k2 = k1;
        k2.id = p.sequence.next_clip_id++;
        k2.tl_in = 90;
        k2.tl_out = 180;
        k2.src_in = 0;
        k2.src_out = 90;
        p.sequence.audio_tracks[0].clips.push_back(k2);

        canvas::gui::test::FakeAudioSink sink;
        canvas::gui::AudioPipeline pipe{sink};
        pipe.set_project(&p);
        pipe.add_media(p.media[0]);
        pipe.open_output();
        pipe.rewind(0, true);
        for (int k = 0; k < 90; ++k) pipe.play_step(k, 1.0 / kFps, false);
        const uint64_t before90 = sink.written_total_;
        for (int k = 90; k < 95; ++k) pipe.play_step(k, 1.0 / kFps, false);

        auto at_written = [&](int64_t written_frame) {
            return std::array<float, 2>{
                sink.all_[static_cast<std::size_t>(written_frame) * kChannels + 0],
                sink.all_[static_cast<std::size_t>(written_frame) * kChannels + 1]};
        };
        auto want_at = [&](int64_t media_frame) {
            return std::array<float, 2>{exp_sample(media_frame, 0),
                                        exp_sample(media_frame, 1)};
        };
        bool boundary_ok = true;
        for (int ch = 0; ch < kChannels; ++ch)
            if (at_written(before90)[ch] != want_at(0)[ch]) boundary_ok = false;
        if (boundary_ok)
            for (int ch = 0; ch < kChannels; ++ch)
                if (at_written(before90 + kPerFrame)[ch] != want_at(kPerFrame)[ch])
                    boundary_ok = false;
        check(boundary_ok, "J: same-media clip boundary feeds K2 from its own source start (no jump)");
    }

    {
        const char* wavI = "/tmp/canvas_audio_pipe_jump.wav";
        check(write_wav(wavI, 20, 300.0, 0.7f, 0.1, 0.6), "I: write jump wav");

        Project p = make_project(0, 600);
        p.media[0].path = wavI;
        p.media[0].total_frames = 600;
        p.sequence.audio_tracks[0].clips[0].tl_out = 600;
        p.sequence.audio_tracks[0].clips[0].src_out = 600;

        canvas::gui::test::FakeAudioSink sink;
        canvas::gui::AudioPipeline pipe{sink};
        pipe.set_project(&p);
        pipe.add_media(p.media[0]);
        pipe.open_output();
        pipe.rewind(0, true);

        const uint64_t leadI = static_cast<uint64_t>(
            static_cast<double>(kRate) * canvas::gui::kAudioLeadMs / 1000.0);
        bool forward_ok = true;
        for (int k = 0; k < 60; ++k) {
            pipe.play_step(k, 1.0 / kFps, false);
            const uint64_t exact = static_cast<uint64_t>(k + 1) * kPerFrame;
            if (sink.written_total_ < exact ||
                sink.written_total_ > exact + leadI + kPerFrame) {
                forward_ok = false;
            }
        }
        check(forward_ok, "I: forward segment writes one frame per step (plus bounded lead)");

        pipe.play_step(30, 1.0 / kFps, false);
        const uint64_t after_jump = sink.written_total_;
        check(after_jump > static_cast<uint64_t>(60) * kPerFrame,
              "I: backward-jump step writes audio (self re-anchor, no silent step)");

        bool resumed_ok = true;
        for (int k = 31; k < 100; ++k) {
            const uint64_t before = sink.written_total_;
            pipe.play_step(k, 1.0 / kFps, false);
            if (sink.written_total_ == before) { resumed_ok = false; break; }
        }
        check(resumed_ok,
              "I: after the un-anchored backward jump playback keeps writing every frame");
        const uint64_t after_exact =
            after_jump + static_cast<uint64_t>(100 - 31) * kPerFrame;
        check(sink.written_total_ >= after_exact - leadI,
              "I: post-jump total stays within the lead window of the step sum");
        check(sink.written_total_ <= after_exact + leadI + kPerFrame,
              "I: post-jump total never overshoots by more than the lead + a step");

        std::remove(wavI);
    }

    {
        const char* capWav = "/tmp/canvas_audio_pipe_capture.wav";
        std::remove(capWav);
        Harness h;
        h.pipe.rewind(0, true);
        h.pipe.set_wave_capture(capWav);
        h.pipe.preroll(0, 70, true);
        h.play(0, 5);
        h.pipe.close_wave_capture();

        std::FILE* f = std::fopen(capWav, "rb");
        check(f != nullptr, "J: capture file exists");
        if (f) {
            std::uint8_t hdr[44];
            std::fread(hdr, 1, 44, f);
            check(std::memcmp(hdr, "RIFF", 4) == 0 && std::memcmp(hdr + 8, "WAVE", 4) == 0,
                  "J: capture is a RIFF/WAVE");
            std::uint32_t riff_sz = 0, data_sz = 0;
            std::fseek(f, 4, SEEK_SET);
            std::fread(&riff_sz, 4, 1, f);
            std::fseek(f, 40, SEEK_SET);
            std::fread(&data_sz, 4, 1, f);
            const std::uint32_t expected =
                static_cast<std::uint32_t>(h.sink.written_total_ * kChannels * 4u);
            check(data_sz == expected && riff_sz == 36 + expected,
                  "J: capture data size matches written frames");
            std::uint16_t fmt_id = 0;
            std::fseek(f, 20, SEEK_SET);
            std::fread(&fmt_id, 2, 1, f);
            check(fmt_id == 3, "J: capture is float32 PCM");
            std::vector<float> samples(8 * kChannels);
            std::fseek(f, 44, SEEK_SET);
            std::fread(samples.data(), sizeof(float), samples.size(), f);
            check(region_matches(samples, 0, samples.size(), 0),
                  "J: capture leading samples match the tone");
            std::fclose(f);
        }
        std::remove(capWav);
    }
    std::remove(wav);

    if (failures) {
        std::fprintf(stderr, "audio_pipeline_test: %d failure(s)\n", failures);
        return 1;
    }
    std::printf("audio_pipeline_test: all passed\n");
    return 0;
}
