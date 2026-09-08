// Round-trip test for the timeline engine and project serialization.
// Builds a sequence via edit ops, saves, loads, and verifies the clip
// layout is identical (playback-identical). Run under ASAN.

#include "canvas/core/media/audio_waveform.hpp"
#include "canvas/core/media/audio_decoder.hpp"
#include "canvas/core/media/video_decoder.hpp"
#include "canvas/core/export/renderer.hpp"
#include "canvas/core/export/render_queue.hpp"
#include "canvas/core/project/project.hpp"
#include "canvas/core/timeline/audio_fade.hpp"
#include "canvas/core/timeline/edit_ops.hpp"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

using namespace canvas::core;

namespace {

bool tracks_equal(const std::vector<Track>& a, const std::vector<Track>& b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        const Track& ta = a[i];
        const Track& tb = b[i];
        if (ta.clips.size() != tb.clips.size()) return false;
        for (std::size_t j = 0; j < ta.clips.size(); ++j) {
            const Clip& ca = ta.clips[j];
            const Clip& cb = tb.clips[j];
            if (ca.media != cb.media || ca.tl_in != cb.tl_in || ca.tl_out != cb.tl_out ||
                ca.src_in != cb.src_in || ca.src_out != cb.src_out || ca.linked_id != cb.linked_id) {
                return false;
            }
        }
    }
    return true;
}

int failures = 0;

void check(bool cond, const char* what) {
    if (!cond) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++failures;
    } else {
        std::printf("ok:   %s\n", what);
    }
}

namespace {
constexpr double kTau = 6.2831853071795865;

// Synthesize a deterministic stereo 16-bit WAV (no encoder needed) so audio
// decoder tests have ground truth without external fixtures. Frame idx n has
// channel c = round(0.8*sin(TAU*(300+100c)/rate*n + start_c)*32767)/32768.
bool write_test_wav(const char* path, int rate, int channels, double seconds) {
    std::FILE* f = std::fopen(path, "wb");
    if (!f) return false;
    const int64_t total = static_cast<int64_t>(rate * seconds);
    const std::uint32_t data_bytes = static_cast<std::uint32_t>(total) * channels * 2;
    const std::uint32_t byte_rate = rate * (channels * 2);
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
    std::fwrite(&rate, 4, 1, f);
    std::fwrite(&byte_rate, 4, 1, f);
    std::fwrite(&block_align, 2, 1, f);
    std::fwrite(&bps, 2, 1, f);
    std::fwrite("data", 1, 4, f);
    std::fwrite(&data_bytes, 4, 1, f);
    const double start_c[2] = {0.0, 1.7};
    for (int64_t i = 0; i < total; ++i) {
        for (int c = 0; c < channels; ++c) {
            const double ph = kTau * (300.0 + 100.0 * c) / rate * static_cast<double>(i) +
                              start_c[c];
            const float v = static_cast<float>(0.8 * std::sin(ph));
            const std::int16_t s = static_cast<std::int16_t>(std::llround(v * 32767.0f));
            std::fwrite(&s, 2, 1, f);
        }
    }
    std::fclose(f);
    return true;
}

// Ground-truth float for the same waveform (must match write_test_wav bit-for-bit).
float test_wav_sample(int64_t frame, int channels_total, int ch) {
    const double ph = kTau * (300.0 + 100.0 * ch) / 48000.0 * static_cast<double>(frame) +
                      (ch ? 1.7 : 0.0);
    const float v = static_cast<float>(0.8 * std::sin(ph));
    return static_cast<float>(std::llround(v * 32767.0f)) / 32768.0f;
}
} // namespace

Project make_project() {
    Project p;
    p.name = "RoundTrip";
    p.sequence.fps = 30.0;

    MediaEntry m0;
    m0.id = 0;
    m0.path = "/tmp/opencode/media/testclip.mp4";
    m0.fps = 30.0;
    m0.width = 1280;
    m0.height = 720;
    m0.total_frames = 300;
    m0.bin = "Scratch";
    p.media.push_back(m0);

    p.bins.push_back("Scratch");

    Track v1;
    v1.kind = Track::Kind::Video;
    v1.name = "V1";
    Track a1;
    a1.kind = Track::Kind::Audio;
    a1.name = "A1";
    p.sequence.video_tracks.push_back(std::move(v1));
    p.sequence.audio_tracks.push_back(std::move(a1));
    return p;
}

}  // namespace

int main() {
    UndoStack undo;

    {
        Project p = make_project();

        // Place clip A overwrite at 0, 60 frames source.
        Clip a;
        a.media = 0;
        a.name = "A";
        a.tl_in = 0;
        a.src_in = 0;
        a.src_out = 60;
        auto cmd = place_clip(p.sequence, Track::Kind::Video, 0, a, Placement::Overwrite);
        check(cmd != nullptr, "place_clip overwrite returns command");
        undo.record(std::move(cmd));

        // Append clip B.
        Clip b;
        b.media = 0;
        b.name = "B";
        b.src_in = 0;
        b.src_out = 90;
        cmd = place_clip(p.sequence, Track::Kind::Video, 0, b, Placement::AppendAtEnd);
        check(cmd != nullptr, "append_clip returns command");
        undo.record(std::move(cmd));

        check(p.sequence.duration_frames() == 150, "duration after two clips is 150");

        // Blade at frame 30 on V1.
        cmd = blade_at(p.sequence, Track::Kind::Video, 0, 30);
        check(cmd != nullptr, "blade_at returns command");
        undo.record(std::move(cmd));
        check(p.sequence.video_tracks[0].clips.size() == 3, "blade splits clip into two (3 clips)");

        // Lift (delete leaving gap) the middle clip (frame 0..30).
        cmd = lift_range(p.sequence, Track::Kind::Video, 0, 0, 30);
        check(cmd != nullptr, "lift_range returns command");
        undo.record(std::move(cmd));
        check(p.sequence.video_tracks[0].clips.size() == 2, "lift removes one clip");
        const int64_t dur_before = p.sequence.duration_frames();
        check(dur_before == 150, "lift keeps total duration (gap left)");

        // Undo the lift: clip comes back.
        check(undo.undo(p.sequence), "undo lift");
        check(p.sequence.video_tracks[0].clips.size() == 3, "after undo, 3 clips again");

        // Redo lift.
        check(undo.redo(p.sequence), "redo lift");
        check(p.sequence.video_tracks[0].clips.size() == 2, "after redo, 2 clips");

        // Ripple delete the range 30..120: removes B entirely and closes gap.
        cmd = ripple_delete_range(p.sequence, Track::Kind::Video, 0, 30, 120);
        check(cmd != nullptr, "ripple_delete_range returns command");
        undo.record(std::move(cmd));
        std::printf("  info: after ripple delete, duration=%lld clips=%zu\n",
                    (long long)p.sequence.duration_frames(), p.sequence.video_tracks[0].clips.size());
        // One 30-frame clip remains starting at 30; its end frame (duration) is 60.
        check(p.sequence.video_tracks[0].clips.size() == 1, "ripple delete leaves one clip");
        check(p.sequence.video_tracks[0].clips[0].duration() == 30, "ripple delete leaves 30-frame clip");
        check(p.sequence.duration_frames() == 60, "ripple delete closes gap (end frame 60)");

        // Serialize.
        std::string err;
        check(save_project(p, "/tmp/opencode/media/roundtrip.ehproj", &err), "save_project");

        Project loaded;
        check(load_project(loaded, "/tmp/opencode/media/roundtrip.ehproj", &err), "load_project");
        check(tracks_equal(p.sequence.video_tracks, loaded.sequence.video_tracks),
              "video tracks identical after round-trip");
        check(p.media.size() == loaded.media.size(), "media registry preserved");
        check(loaded.media.size() == 1 && loaded.media[0].bin == "Scratch", "media bin preserved");
        check(loaded.bins.size() == 1 && loaded.bins[0] == "Scratch", "bins list preserved");

        // Undo to the state just before the ripple delete (2 clips, duration 150).
        check(undo.undo(p.sequence), "undo ripple delete");
        check(p.sequence.video_tracks[0].clips.size() == 2, "after undo ripple, 2 clips");
        check(p.sequence.duration_frames() == 150, "after undo ripple, duration 150");

        // Undo everything from the full history to an empty track.
        while (undo.undo(p.sequence)) {}
        check(p.sequence.video_tracks[0].clips.empty(), "unwound to empty track");
    }

    {   // Blading a fade-bearing clip must not plant a transition on the seam.
        Project p = make_project();
        Clip a;
        a.media = 0;
        a.name = "FadeOut";
        a.tl_in = 0;
        a.src_in = 0;
        a.src_out = 60;
        auto cmd = place_clip(p.sequence, Track::Kind::Video, 0, a, Placement::Overwrite);
        check(cmd != nullptr, "transition-blade: place clip");
        undo.record(std::move(cmd));

        const ClipId a_id = p.sequence.video_tracks[0].clips[0].id;
        cmd = set_clip_transition(p.sequence, Track::Kind::Video, 0, a_id,
                                  TransitionType::CrossDissolve, 14);
        check(cmd != nullptr, "transition-blade: add OUT fade");
        undo.record(std::move(cmd));
        check(p.sequence.video_tracks[0].clips[0].has_transition_out(),
              "transition-blade: fade is stored on the clip");

        cmd = blade_at(p.sequence, Track::Kind::Video, 0, 15);
        check(cmd != nullptr, "transition-blade: cut mid-fade clip");
        undo.record(std::move(cmd));

        const auto& clips = p.sequence.video_tracks[0].clips;
        check(clips.size() == 2, "transition-blade: clip split in two");
        if (clips.size() == 2) {
            // Left half [0,15): its tail is now an interior cut -> no OUT fade.
            check(!clips[0].has_transition_out(), "transition-blade: seam is a plain cut (left half)");
            // Right half [15,60): head is an interior cut -> no IN fade; its tail
            // (the ORIGINAL clip end) keeps the fade-out.
            check(!clips[1].has_transition_in(), "transition-blade: seam is a plain cut (right half)");
            check(clips[1].has_transition_out() && clips[1].transition_out_duration == 14,
                  "transition-blade: fade stays on the original tail");
        }
    }

    {   // Linked audio/video placement, movement, deletion, and unlink.
        Project p = make_project();

        Clip v;
        v.media = 0;
        v.name = "V";
        v.tl_in = 0;
        v.src_in = 0;
        v.src_out = 60;
        Clip a = v;
        auto cmd = place_linked_clip(p.sequence, 0, 0, v, a, Placement::Overwrite);
        check(cmd != nullptr, "place_linked_clip returns command");
        undo.record(std::move(cmd));

        const auto& vc = p.sequence.video_tracks[0].clips[0];
        const auto& ac = p.sequence.audio_tracks[0].clips[0];
        check(vc.is_linked() && ac.is_linked(), "linked pair is mutually linked");
        check(vc.linked_id == ac.id && ac.linked_id == vc.id, "reciprocal linked ids");

        // Move the video clip to frame 30; the audio mate should follow.
        cmd = move_clip(p.sequence, Track::Kind::Video, 0, vc.id,
                        Track::Kind::Video, 0, 30);
        check(cmd != nullptr, "move linked video returns command");
        undo.record(std::move(cmd));
        check(p.sequence.video_tracks[0].clips[0].tl_in == 30 &&
              p.sequence.audio_tracks[0].clips[0].tl_in == 30,
              "moving video moves audio mate");

        // Delete the video clip (lift its range); audio mate should be removed too.
        const auto& mv = p.sequence.video_tracks[0].clips[0];
        cmd = lift_range(p.sequence, Track::Kind::Video, 0, mv.tl_in, mv.tl_out);
        check(cmd != nullptr, "lift linked video returns command");
        undo.record(std::move(cmd));
        check(p.sequence.video_tracks[0].clips.empty() && p.sequence.audio_tracks[0].clips.empty(),
              "deleting video removes linked audio");

        // Undo back to the linked pair, then unlink.
        undo.undo(p.sequence);
        undo.undo(p.sequence);
        const auto& uvc = p.sequence.video_tracks[0].clips[0];
        cmd = unlink_clip(p.sequence, Track::Kind::Video, 0, uvc.id);
        check(cmd != nullptr, "unlink returns command");
        undo.record(std::move(cmd));
        check(!p.sequence.video_tracks[0].clips[0].is_linked() &&
              !p.sequence.audio_tracks[0].clips[0].is_linked(),
              "unlink clears both sides");

        // Moving the now-unlinked video must NOT move audio.
        const auto& uc = p.sequence.video_tracks[0].clips[0];
        cmd = move_clip(p.sequence, Track::Kind::Video, 0, uc.id, Track::Kind::Video, 0, 40);
        check(cmd != nullptr, "move unlinked video returns command");
        undo.record(std::move(cmd));
        check(p.sequence.video_tracks[0].clips[0].tl_in == 40 &&
              p.sequence.audio_tracks[0].clips[0].tl_in == 0,
              "unlinked video moves independently of audio");

        // Link the unlinked video clip back to the overlapping audio clip.
        const auto& relink = p.sequence.video_tracks[0].clips[0];
        cmd = link_clip(p.sequence, Track::Kind::Video, 0, relink.id);
        check(cmd != nullptr, "link_clip returns command");
        undo.record(std::move(cmd));
        check(p.sequence.video_tracks[0].clips[0].is_linked() &&
              p.sequence.audio_tracks[0].clips[0].is_linked(),
              "link_clip re-links both sides");

        // link_clip on an already-linked clip is a no-op.
        cmd = link_clip(p.sequence, Track::Kind::Video, 0, relink.id);
        check(cmd == nullptr, "link_clip on linked clip returns nullptr");
    }

    {   // Moving a linked video clip to another video channel keeps its audio mate.
        Project p = make_project();

        Clip v;
        v.media = 0;
        v.name = "V";
        v.tl_in = 0;
        v.src_in = 0;
        v.src_out = 60;
        Clip a = v;
        auto cmd = place_linked_clip(p.sequence, 0, 0, v, a, Placement::Overwrite);
        check(cmd != nullptr, "place_linked_clip (cross-track) returns command");
        undo.record(std::move(cmd));

        // Add a second video channel (V2).
        Track v2;
        v2.kind = Track::Kind::Video;
        v2.name = "V2";
        p.sequence.video_tracks.push_back(std::move(v2));

        const auto& vc = p.sequence.video_tracks[0].clips[0];
        cmd = move_clip(p.sequence, Track::Kind::Video, 0, vc.id,
                        Track::Kind::Video, 1, 30);
        check(cmd != nullptr, "move linked video to V2 returns command");
        undo.record(std::move(cmd));

        check(p.sequence.video_tracks[1].clips.size() == 1, "video moved to V2");
        check(p.sequence.video_tracks[0].clips.empty(), "video removed from V1");
        check(p.sequence.audio_tracks[0].clips.size() == 1, "audio mate survives on A1");
        check(p.sequence.audio_tracks[0].clips[0].tl_in == 30, "audio mate follows timing");
        check(p.sequence.video_tracks[1].clips[0].is_linked() &&
              p.sequence.audio_tracks[0].clips[0].is_linked(),
              "link preserved across channels");
    }

    {
        // reduce_waveform: re-bucket a high-resolution waveform down to a
        // target pixel width without re-decoding. This backs the thumbnail
        // service's "decode once per media, re-bucket per width" optimization.
        AudioWaveform raw;
        raw.buckets = 4096;
        raw.peak.assign(4096, 0.0f);
        raw.rms.assign(4096, 0.0f);
        raw.duration_seconds = 100.0;
        for (std::size_t i = 0; i < raw.buckets; ++i)
            raw.peak[i] = 0.1f + 0.8f * static_cast<float>(i) / float(raw.buckets);

        AudioWaveform out = reduce_waveform(raw, 128);
        check(out.buckets == 128, "reduce_waveform yields requested bucket count");
        check(out.peak.size() == 128 && out.rms.size() == 128,
              "reduce_waveform peak/rms arrays sized to target");
        check(out.duration_seconds == 100.0, "reduce_waveform preserves duration");
        check(out.peak[0] >= raw.peak[0] - 1e-6f && out.peak[0] <= 1.0f,
              "reduce_waveform first peak is max of its range, within [0,1]");
        check(out.peak.back() >= out.peak.front(), "reduce_waveform preserves rising trend");

        // Last target bucket should approximate the max of the last source
        // buckets (peak semantics: max within range).
        float last_peak = 0.0f;
        for (std::size_t i = 0; i < raw.buckets; ++i)
            last_peak = std::max(last_peak, raw.peak[i]);
        check(out.peak.back() >= last_peak - 1e-6f,
              "reduce_waveform peak reflects max of source range");

        // Degenerate inputs must not crash or produce wrong sizes.
        AudioWaveform empty = reduce_waveform(AudioWaveform{}, 64);
        check(empty.buckets == 64 && empty.peak.size() == 64,
              "reduce_waveform handles empty source safely");
        AudioWaveform zero = reduce_waveform(raw, 0);
        check(zero.buckets == 0 && zero.peak.empty(), "reduce_waveform handles 0 target");

        // Range-aware variant: a clip displaying a sub-window of the media must
        // see exactly that slice. Sample the source at the mid-point (peak array
        // scales linearly 0.1..0.9) and split the window in half.
        const double mid = raw.peak[raw.buckets / 2];
        AudioWaveform lo = reduce_waveform(raw, 8, 0.0, 0.5);
        AudioWaveform hi = reduce_waveform(raw, 8, 0.5, 1.0);
        check(lo.buckets == 8 && hi.buckets == 8,
              "reduce_waveform range yields requested bucket count");
        // The lo slice only samples raw buckets below the midline, so its last
        // bucket peak can never rise past it (peak = max over the slice).
        check(lo.peak.back() <= mid + 1e-6f && lo.peak.back() >= lo.peak.front(),
              "reduce_waveform lo-half stays within its half and rises");
        // The hi slice samples only at/above the midline, so its first bucket
        // peak stays at/above it; its last covers the tail and hits the max.
        check(hi.peak.front() >= mid - 1e-6f,
              "reduce_waveform hi-half begins at the midline peak");
        check(hi.peak.back() >= raw.peak.back() - 1e-6f,
              "reduce_waveform hi-half ends at the source maximum");
        AudioWaveform whole = reduce_waveform(raw, 8, 0.0, 1.0);
        check(whole.peak.back() >= lo.peak.back() && whole.peak.front() <= hi.peak.front(),
              "reduce_waveform whole-file spans the range pieces");
        // Degenerate range falls back to whole-file rather than blanking.
        AudioWaveform bad = reduce_waveform(raw, 4, 0.5, 0.5);
        check(bad.peak[0] >= raw.peak[0], "reduce_waveform zero-width range is safe");

        // Sanity: 2-arg and 3-arg whole-file forms agree.
        AudioWaveform all = reduce_waveform(raw, 128, 0.0, 1.0);
        check(all.peak == out.peak && all.rms == out.rms,
              "reduce_waveform 3-arg full range matches 2-arg");
    }

    {
        // Low-resolution preview decode: decode_to_frame(..., max_output_dim)
        // must return a scaled-down RGBA frame while the full-res path keeps
        // native size. This backs fast scrubbing without poisoning the
        // full-res playback cache.
        VideoDecoder dec;
        std::string err;
        if (dec.open("/tmp/opencode/test_av.mp4", &err)) {
            auto full = dec.decode_to_frame(5, 0);
            check(full && full->width > 0 && full->height > 0,
                  "decode_to_frame full-res decodes at native size");
            if (full) {
                check(full->width >= full->height &&
                          static_cast<int>(full->width * full->height) > 0,
                      "decode_to_frame full-res has valid geometry");
            }

            auto preview = dec.decode_to_frame(20, 64);
            check(preview && preview->width > 0 && preview->height > 0,
                  "decode_to_frame low-res decodes a frame");
            if (preview) {
                const int longest = std::max(preview->width, preview->height);
                check(longest <= 64, "decode_to_frame low-res caps longest edge");
                check(preview->width < full->width,
                      "decode_to_frame low-res is smaller than full-res");
                check(!preview->rgba.empty(), "decode_to_frame low-res has pixels");
            }
        } else {
            std::printf("  (skipping low-res decode test: test media unavailable)\n");
        }
    }

    {
        // Audio decoder seek + sequential-advance must produce real (non-silent)
        // samples that move forward in time. Two regression guards:
        //   1. Seeking by the audio stream index on PCM/AAC in a container used
        //      to return only silent frames ("no audio after scrubbing") ->
        //      seek by the file-wide timestamp (stream -1) instead.
        //   2. After a seek, decoded_at was left 0, so the next call re-sought
        //      and served the identical first slice forever (repeating audio)
        //      -> decoded_at must be anchored at the seek target.
        AudioDecoder adec;
        std::string aerr;
        // Test media with an audio stream. Prefer the generated AV clip.
        const char* apath = "/tmp/opencode/test_av.mp4";
        if (adec.open(apath) && adec.has_audio()) {
            const int out_rate = 44100;
            // Far forward seek (like a scrub), then small sequential steps that
            // mirror playback resuming after the scrub lands.
            const int64_t base = static_cast<int64_t>(out_rate) * 20LL;
            auto a0 = adec.decode(base, 800, out_rate);
            check(a0 && !a0->samples.empty(), "audio decoder far seek returns frames");
            if (a0) {
                float peak = 0.0f;
                for (const float s : a0->samples) {
                    const float a = s < 0.0f ? -s : s;
                    if (a > peak) peak = a;
                }
                // Peak above an absurdly-low threshold proves real signal, not
                // the float-rounding denormals a broken seek produces.
                check(peak > 1e-4f,
                      "audio decoder far seek yields non-silent samples");
                // If a sequential call reports a start_sample far behind the
                // requested position (or returns the same bytes), the decoder
                // is stuck re-serving the first slice instead of advancing.
                auto a1 = adec.decode(base + 800, 800, out_rate);
                check(a1 && a1->start_sample == base + 800 &&
                          !a1->samples.empty(),
                      "audio decoder advances after seek");
                if (a1) {
                    float p1 = 0.0f;
                    for (const float s : a1->samples) {
                        const float a = s < 0.0f ? -s : s;
                        if (a > p1) p1 = a;
                    }
                    check(peak > 0.0f && p1 > 0.0f &&
                              a1->samples != a0->samples,
                          "audio decoder does not repeat after seek");
                }
            }
        } else {
            std::printf("  (skipping audio decoder seek test: test media unavailable)\n");
        }
    }

    {
        // AudioDecoder forward-jump regression (trimmed-head fix): before the
        // fix, decode() only hard-seeked on BACKWARD jumps, so a request well
        // FORWARD of the current position silently served the file's beginning
        // until sequential decode caught up — audible as ~1s of garbled audio
        // at the head of any clip trimmed from its start. Uses a synthesized
        // deterministic WAV so the served region can be asserted exactly.
        const char* wpath = "/tmp/canvas_audio_fw_test.wav";
        const bool wrote = write_test_wav(wpath, 48000, 2, 6);
        check(wrote, "fw-jump: write test wav");
        if (!wrote) return 1;
        constexpr int kRate = 48000;
        constexpr int64_t kHead = 51200;  // e.g. a 32-frame trim at 30fps
        constexpr int kSpan = 800;
        std::vector<float> truth;
        {
            AudioDecoder ref;
            check(ref.open(wpath) && ref.has_audio(), "fw-jump: open wav");
            int64_t pos = 0;
            while (pos < kHead + kSpan) {
                auto c = ref.decode(pos, kSpan, kRate);
                if (!c || c->samples.empty()) break;
                truth.insert(truth.end(), c->samples.begin(), c->samples.end());
                pos += static_cast<int64_t>(c->samples.size()) / c->channels;
            }
            check(truth.size() >= static_cast<std::size_t>(kHead + kSpan) * 2,
                  "fw-jump: sequential reference decodes the full region");
        }
        const auto region_matches = [&](const std::vector<float>& s, int64_t start_frame) {
            if (s.size() < static_cast<std::size_t>(kSpan) * 2) return false;
            for (int i = 0; i < kSpan * 2; ++i) {
                if (std::fabs(s[i] - test_wav_sample(start_frame + i / 2, 2, i % 2)) > 1e-3f)
                    return false;
            }
            return true;
        };
        // Path 1: fresh decoder, first request lands at the trimmed head.
        {
            AudioDecoder a;
            check(a.open(wpath) && a.has_audio(), "fw-jump: open decoder A");
            auto c1 = a.decode(kHead, kSpan, kRate);
            check(c1 && c1->samples.size() >= static_cast<std::size_t>(kSpan) * 2,
                  "fw-jump: fresh first call at the head serves frames");
            check(region_matches(c1 ? c1->samples : std::vector<float>{}, kHead),
                  "fw-jump: fresh first call serves the requested region, not file start");
        }
        // Path 2: playback-rewind shape — decode ahead a while, reset(), then
        // jump well forward again.
        {
            AudioDecoder b;
            check(b.open(wpath) && b.has_audio(), "fw-jump: open decoder B");
            int64_t q = 0;
            for (int n = 0; n < 200 && q < kHead; ++n) {
                auto cc = b.decode(q, kSpan, kRate);
                if (!cc) break;
                q += static_cast<int64_t>(cc->samples.size()) / cc->channels;
            }
            b.reset();
            auto c2 = b.decode(kHead, kSpan, kRate);
            check(c2 && c2->samples.size() >= static_cast<std::size_t>(kSpan) * 2,
                  "fw-jump: post-rewind request at the head serves frames");
            check(region_matches(c2 ? c2->samples : std::vector<float>{}, kHead),
                  "fw-jump: post-rewind request serves the requested region");
        }
        std::remove(wpath);
    }

    {
        // Transitions: set/clear a clip's OUT transition (type + duration),
        // and verify it round-trips through project serialization. The
        // transition is stored on the clip, so the snapshot-based undo also
        // restores it.
        Project p = make_project();

        Clip a;
        a.media = 0;
        a.name = "A";
        a.tl_in = 0;
        a.src_in = 0;
        a.src_out = 30;
        auto cmd = place_clip(p.sequence, Track::Kind::Video, 0, a, Placement::Overwrite);
        check(cmd != nullptr, "transition: place clip A");
        undo.record(std::move(cmd));

        Clip b;
        b.media = 0;
        b.name = "B";
        b.tl_in = 30;
        b.src_in = 0;
        b.src_out = 30;
        cmd = place_clip(p.sequence, Track::Kind::Video, 0, b, Placement::Overwrite);
        check(cmd != nullptr, "transition: place clip B");
        undo.record(std::move(cmd));

        const ClipId id_a = p.sequence.video_tracks[0].clips[0].id;

        cmd = set_clip_transition(p.sequence, Track::Kind::Video, 0, id_a,
                                  TransitionType::CrossDissolve, 6);
        check(cmd != nullptr, "set_clip_transition returns command");
        undo.record(std::move(cmd));
        const Clip& ca = p.sequence.video_tracks[0].clips[0];
        check(ca.transition_out == TransitionType::CrossDissolve &&
                  ca.transition_out_duration == 6,
              "clip carries cross-dissolve transition of 6 frames");

        // Undo restores the prior (none) transition state.
        check(undo.undo(p.sequence), "undo transition");
        check(p.sequence.video_tracks[0].clips[0].transition_out == TransitionType::None,
              "undo clears transition");

        // Redo reapplies it, then serialize and back.
        check(undo.redo(p.sequence), "redo transition");
        cmd = clear_clip_transition(p.sequence, Track::Kind::Video, 0, id_a);
        check(cmd != nullptr, "clear_clip_transition returns command");
        undo.record(std::move(cmd));
        check(p.sequence.video_tracks[0].clips[0].transition_out == TransitionType::None,
              "clear removes transition");
        // Restore transition for the round-trip check.
        cmd = set_clip_transition(p.sequence, Track::Kind::Video, 0, id_a,
                                  TransitionType::WipeRight, 8);
        check(cmd != nullptr, "set wipe transition returns command");
        undo.record(std::move(cmd));

        std::string tr_err;
        check(save_project(p, "/tmp/opencode/media/transition.ehproj", &tr_err), "save project with transition");
        Project tr_loaded;
        check(load_project(tr_loaded, "/tmp/opencode/media/transition.ehproj", &tr_err), "load project with transition");
        const auto& trc = tr_loaded.sequence.video_tracks[0].clips[0];
        check(trc.transition_out == TransitionType::WipeRight && trc.transition_out_duration == 8,
              "transition type+duration round-trip through serialization");

        // IN (leading-edge) transition: independent of OUT, fades the clip in
        // from black at its head. Verify set / undo / clear / serialization.
        cmd = set_clip_transition_in(p.sequence, Track::Kind::Video, 0, id_a,
                                     TransitionType::FadeIn, 12);
        check(cmd != nullptr, "set_clip_transition_in returns command");
        undo.record(std::move(cmd));
        check(p.sequence.video_tracks[0].clips[0].transition_in == TransitionType::FadeIn &&
                  p.sequence.video_tracks[0].clips[0].transition_in_duration == 12,
              "clip carries IN fade-in transition of 12 frames");
        check(undo.undo(p.sequence), "undo IN transition");
        check(p.sequence.video_tracks[0].clips[0].transition_in == TransitionType::None,
              "undo clears IN transition");
        check(undo.redo(p.sequence), "redo IN transition");
        cmd = clear_clip_transition_in(p.sequence, Track::Kind::Video, 0, id_a);
        check(cmd != nullptr, "clear_clip_transition_in returns command");
        undo.record(std::move(cmd));
        check(p.sequence.video_tracks[0].clips[0].transition_in == TransitionType::None &&
                  p.sequence.video_tracks[0].clips[0].transition_out == TransitionType::WipeRight &&
                  p.sequence.video_tracks[0].clips[0].transition_out_duration == 8,
              "clearing IN does not disturb OUT transition");
        cmd = set_clip_transition_in(p.sequence, Track::Kind::Video, 0, id_a,
                                     TransitionType::FadeIn, 12);
        check(cmd != nullptr, "restore IN transition for round-trip");
        undo.record(std::move(cmd));
        std::string ierr;
        check(save_project(p, "/tmp/opencode/media/transition_in.ehproj", &ierr),
              "save project with IN transition");
        Project in_loaded;
        check(load_project(in_loaded, "/tmp/opencode/media/transition_in.ehproj", &ierr),
              "load project with IN transition");
        const auto& in_c = in_loaded.sequence.video_tracks[0].clips[0];
        check(in_c.transition_in == TransitionType::FadeIn && in_c.transition_in_duration == 12,
              "IN transition type+duration round-trip through serialization");
        check(in_c.transition_out == TransitionType::WipeRight && in_c.transition_out_duration == 8,
              "OUT transition survives IN-round-trip");

        // Linked audio mate inherits the video transition.
        Project lp = make_project();
        Clip v;
        v.media = 0;
        v.tl_in = 0;
        v.src_in = 0;
        v.src_out = 30;
        Clip aud = v;
        cmd = place_linked_clip(lp.sequence, 0, 0, v, aud, Placement::Overwrite);
        check(cmd != nullptr, "transition: place linked pair");
        undo.record(std::move(cmd));
        const ClipId vid = lp.sequence.video_tracks[0].clips[0].id;
        cmd = set_clip_transition(lp.sequence, Track::Kind::Video, 0, vid,
                                  TransitionType::AudioFadeConstantPower, 10);
        check(cmd != nullptr, "transition: set on linked keyword returns command");
        undo.record(std::move(cmd));
        check(lp.sequence.video_tracks[0].clips[0].transition_out == TransitionType::AudioFadeConstantPower &&
                  lp.sequence.audio_tracks[0].clips[0].transition_out == TransitionType::AudioFadeConstantPower,
              "linked audio mate inherits the transition type");
        check(lp.sequence.audio_tracks[0].clips[0].transition_out_duration == 10,
              "linked audio mate inherits the transition duration");

        // Per-kind type translation: a video transition on a linked pair must put
        // a matching AUDIO fade on the audio mate (the requested behavior), not
        // the same video-only type (which the audio mixers would ignore).
        cmd = set_clip_transition(lp.sequence, Track::Kind::Video, 0, vid,
                                  TransitionType::CrossDissolve, 10);
        undo.record(std::move(cmd));
        check(lp.sequence.video_tracks[0].clips[0].transition_out == TransitionType::CrossDissolve &&
                  lp.sequence.audio_tracks[0].clips[0].transition_out ==
                      TransitionType::AudioFadeConstantPower,
              "video cross dissolve translates the audio mate to an equal-power fade");
        cmd = set_clip_transition_in(lp.sequence, Track::Kind::Video, 0, vid,
                                     TransitionType::DipToBlack, 10);
        undo.record(std::move(cmd));
        check(lp.sequence.video_tracks[0].clips[0].transition_in == TransitionType::DipToBlack &&
                  lp.sequence.audio_tracks[0].clips[0].transition_in ==
                      TransitionType::AudioFadeConstantGain,
              "dip-to-black translates the audio mate to a dip to silence");
        // ...and the reverse direction: an audio fade applied via the AUDIO clip
        // gives the video mate a matching video transition.
        const ClipId aud_id = lp.sequence.audio_tracks[0].clips[0].id;
        cmd = set_clip_transition(lp.sequence, Track::Kind::Audio, 0, aud_id,
                                  TransitionType::AudioFadeConstantGain, 8);
        undo.record(std::move(cmd));
        check(lp.sequence.audio_tracks[0].clips[0].transition_out ==
                  TransitionType::AudioFadeConstantGain &&
                  lp.sequence.video_tracks[0].clips[0].transition_out == TransitionType::FadeOut,
              "audio fade out translates the video mate to FadeOut");
    }

    {
        // Render-time audio fade: the exported mix must actually apply the clip's
        // AUDIO IN/OUT transitions (audio_fade_gain envelope). Uses the
        // synthesized WAV as media so the expected waveform is exact ground truth.
        const char* wpath = "/tmp/canvas_audio_fade_test.wav";
        check(write_test_wav(wpath, 48000, 2, 3), "render-fade: write test wav");
        Project p = make_project();
        p.media[0].path = wpath;
        p.media[0].fps = 30.0;
        Clip a;
        a.media = 0;
        a.tl_in = 0;
        a.src_in = 0;
        a.src_out = 30;
        auto cmd = place_clip(p.sequence, Track::Kind::Audio, 0, a, Placement::Overwrite);
        check(cmd != nullptr, "render-fade: place audio clip");
        undo.record(std::move(cmd));
        const ClipId id = p.sequence.audio_tracks[0].clips[0].id;
        cmd = set_clip_transition(p.sequence, Track::Kind::Audio, 0, id,
                                  TransitionType::AudioFadeConstantGain, 6);
        undo.record(std::move(cmd));
        const Clip& fade_clip = p.sequence.audio_tracks[0].clips[0];

        constexpr int kRate = 48000;
        auto probe = [&](int64_t start_tl, int frames,
                         const char* what) {
            auto c = render_audio_chunk(p, start_tl * kRate / 30, frames, kRate, 2, 30.0,
                                        nullptr);
            bool ok = c && c->samples.size() == static_cast<std::size_t>(frames) * 2;
            const int64_t base = start_tl * kRate / 30;
            if (c) {
                for (std::size_t s = 0; s < c->samples.size() && ok; ++s) {
                    const int k = static_cast<int>(s / 2);
                    const int64_t tl = start_tl + k / (kRate / 30);
                    const float g = audio_fade_gain(fade_clip, tl);
                    const float want = test_wav_sample(base + s / 2, 2, s % 2) * g;
                    if (std::fabs(c->samples[s] - want) > 1e-3f) ok = false;
                }
            }
            check(ok, what);
        };
        // Before the OUT fade window [24,30): unity gain.
        probe(20, 4, "render-fade: audio before the fade window is unchanged");
        // Inside the window: gain ramps 1 -> 0 (linear ConstantGain).
        probe(25, 10, "render-fade: ConstantGain out-fade scales the exported mix");
        // Overlapping the fade start: first in-window frame already attenuated.
        probe(25, 1, "render-fade: fade window start is attenuated");
        std::remove(wpath);
    }

    {
        // Delete Through Edit: merging two adjacent same-media clips (with
        // continuous source) into a single clip that spans both.
        Project p = make_project();

        Clip a;
        a.media = 0;
        a.name = "A";
        a.tl_in = 0;
        a.src_in = 0;
        a.src_out = 30;
        auto cmd = place_clip(p.sequence, Track::Kind::Video, 0, a, Placement::Overwrite);
        check(cmd != nullptr, "through edit: place clip A");
        undo.record(std::move(cmd));

        // B continues A's source exactly (B.src_in == A.src_out == 30).
        Clip b;
        b.media = 0;
        b.name = "B";
        b.tl_in = 30;
        b.src_in = 30;
        b.src_out = 60;
        cmd = place_clip(p.sequence, Track::Kind::Video, 0, b, Placement::Overwrite);
        check(cmd != nullptr, "through edit: place clip B");
        undo.record(std::move(cmd));

        const ClipId id_a = p.sequence.video_tracks[0].clips[0].id;

        cmd = delete_through_edit(p.sequence, Track::Kind::Video, 0, id_a);
        check(cmd != nullptr, "delete_through_edit returns a command for valid cut");
        undo.record(std::move(cmd));
        const auto& track = p.sequence.video_tracks[0];
        check(track.clips.size() == 1, "through edit merges into one clip");
        check(track.clips[0].tl_in == 0 && track.clips[0].tl_out == 60,
              "merged clip spans [0, 60)");
        check(track.clips[0].src_in == 0 && track.clips[0].src_out == 60,
              "merged clip spans source [0, 60)");

        // Undo restores the two-clip cut.
        check(undo.undo(p.sequence), "undo through edit");
        check(p.sequence.video_tracks[0].clips.size() == 2,
              "undo restores the two clips separated by a cut");
        check(undo.redo(p.sequence), "redo through edit");
        check(p.sequence.video_tracks[0].clips.size() == 1,
              "redo re-merges into one clip");

        // A cut whose clips do not share continuous source is NOT a valid
        // through edit (returns nullptr).
        Project q = make_project();
        Clip c2;
        c2.media = 0;
        c2.tl_in = 0;
        c2.src_in = 0;
        c2.src_out = 30;
        cmd = place_clip(q.sequence, Track::Kind::Video, 0, c2, Placement::Overwrite);
        undo.record(std::move(cmd));
        Clip d2;
        d2.media = 0;  // non-continuous: B starts at src 0, not 30
        d2.tl_in = 30;
        d2.src_in = 0;
        d2.src_out = 30;
        cmd = place_clip(q.sequence, Track::Kind::Video, 0, d2, Placement::Overwrite);
        undo.record(std::move(cmd));
        const ClipId id_c = q.sequence.video_tracks[0].clips[0].id;
        cmd = delete_through_edit(q.sequence, Track::Kind::Video, 0, id_c);
        check(cmd == nullptr, "non-continuous cut is not a valid through edit");
    }

    {
        // Auto-create top tracks: a linked A/V pair on V1/A1 dragged to the
        // top gains a fresh video + audio channel above and hops onto it as a
        // single undoable edit.
        Project p = make_project();
        Clip v;
        v.media = 0;
        v.name = "A";
        v.tl_in = 10;
        v.tl_out = 70;
        v.src_in = 0;
        v.src_out = 60;
        Clip av;
        av.media = 0;
        av.name = "AA";
        av.tl_in = 10;
        av.tl_out = 70;
        av.src_in = 0;
        av.src_out = 60;
        auto cmd = place_clip(p.sequence, Track::Kind::Video, 0, v, Placement::Overwrite);
        check(cmd != nullptr, "auto-track: place video");
        undo.record(std::move(cmd));
        cmd = place_clip(p.sequence, Track::Kind::Audio, 0, av, Placement::Overwrite);
        check(cmd != nullptr, "auto-track: place audio mate");
        undo.record(std::move(cmd));
        const ClipId v_id = p.sequence.video_tracks[0].clips[0].id;
        const ClipId a_id = p.sequence.audio_tracks[0].clips[0].id;
        p.sequence.video_tracks[0].clips[0].linked_id = a_id;
        p.sequence.audio_tracks[0].clips[0].linked_id = v_id;

        const std::size_t v_before = p.sequence.video_tracks.size();
        const std::size_t a_before = p.sequence.audio_tracks.size();
        cmd = create_top_track_move(p.sequence, v_id, 20);
        check(cmd != nullptr, "auto-track: command created");
        check(p.sequence.video_tracks.size() == v_before + 1,
              "auto-track: one video channel added");
        check(p.sequence.audio_tracks.size() == a_before + 1,
              "auto-track: one audio channel added");
        check(p.sequence.video_tracks[0].clips.size() == 1 &&
                  p.sequence.video_tracks[0].clips[0].id == v_id &&
                  p.sequence.video_tracks[0].clips[0].tl_in == 20,
              "auto-track: video clip moved to new top video track at tl_in 20");
        check(p.sequence.audio_tracks[0].clips.size() == 1 &&
                  p.sequence.audio_tracks[0].clips[0].id == a_id,
              "auto-track: audio mate moved to new top audio track");
        check(p.sequence.video_tracks[0].clips[0].tl_in ==
                  p.sequence.audio_tracks[0].clips[0].tl_in,
              "auto-track: A/V sync preserved after move");
        check(p.sequence.video_tracks[1].clips.empty() &&
                  p.sequence.audio_tracks[1].clips.empty(),
              "auto-track: original lanes emptied");
        undo.record(std::move(cmd));

        check(undo.undo(p.sequence), "auto-track: undo");
        check(p.sequence.video_tracks.size() == v_before + 1 &&
                  p.sequence.audio_tracks.size() == a_before + 1,
              "auto-track: undo keeps the new channels (matches track UX)");
        check(p.sequence.video_tracks[1].clip_with_id(v_id) != nullptr &&
                  p.sequence.audio_tracks[1].clip_with_id(a_id) != nullptr,
              "auto-track: undo restores clips to original lanes");
        check(p.sequence.video_tracks[1].clip_with_id(v_id)->tl_in == 10 &&
                  p.sequence.audio_tracks[1].clip_with_id(a_id)->tl_in == 10,
              "auto-track: undo restores original positions");
        check(p.sequence.video_tracks[0].clips.empty() &&
                  p.sequence.audio_tracks[0].clips.empty(),
              "auto-track: undo empties the new lanes");

        check(undo.redo(p.sequence), "auto-track: redo");
        check(p.sequence.video_tracks[0].clip_with_id(v_id) != nullptr &&
                  p.sequence.video_tracks[0].clip_with_id(v_id)->tl_in == 20,
              "auto-track: redo re-moves clip to new lane");

        cmd = create_top_track_move(p.sequence, 999999, 0);
        check(cmd == nullptr, "auto-track: unknown clip id returns nullptr");
    }

    {
        // Per-clip audio mix (volume_db/pan) + track mixing flags (muted/solo)
        // survive a save/load round-trip and undo through the edit ops.
        Project p = make_project();
        Clip a;
        a.media = 0;
        a.tl_in = 0;
        a.src_in = 0;
        a.src_out = 30;
        auto cmd = place_clip(p.sequence, Track::Kind::Audio, 0, a, Placement::Overwrite);
        check(cmd != nullptr, "audio-mix: place audio clip");
        undo.record(std::move(cmd));
        const ClipId id = p.sequence.audio_tracks[0].clips[0].id;

        cmd = set_clip_audio(p.sequence, Track::Kind::Audio, 0, id, -6.0f, 0.75f);
        check(cmd != nullptr, "audio-mix: set_clip_audio returns command");
        undo.record(std::move(cmd));
        const auto& ac0 = p.sequence.audio_tracks[0].clips[0];
        check(ac0.volume_db == -6.0f && ac0.pan == 0.75f,
              "audio-mix: clip carries volume -6 dB and pan 0.75");

        cmd = set_track_muted(p.sequence, Track::Kind::Audio, 0, true);
        check(cmd != nullptr, "audio-mix: set_track_muted returns command");
        undo.record(std::move(cmd));
        cmd = set_track_solo(p.sequence, Track::Kind::Audio, 0, true);
        check(cmd != nullptr, "audio-mix: set_track_solo returns command");
        undo.record(std::move(cmd));
        check(p.sequence.audio_tracks[0].muted && p.sequence.audio_tracks[0].solo,
              "audio-mix: track carries muted+solo");

        cmd = set_track_gain(p.sequence, Track::Kind::Audio, 0, -9.0f);
        check(cmd != nullptr, "audio-mix: set_track_gain returns command");
        undo.record(std::move(cmd));
        check(p.sequence.audio_tracks[0].gain_db == -9.0f,
              "audio-mix: track carries gain -9 dB");

        std::string err;
        check(save_project(p, "/tmp/opencode/media/audio_mix.ehproj", &err),
              "audio-mix: save project");
        Project loaded;
        check(load_project(loaded, "/tmp/opencode/media/audio_mix.ehproj", &err),
              "audio-mix: load project");
        const auto& lc = loaded.sequence.audio_tracks[0].clips[0];
        check(lc.volume_db == -6.0f && lc.pan == 0.75f,
              "audio-mix: clip volume/pan round-trip through serialization");
        check(loaded.sequence.audio_tracks[0].muted &&
                  loaded.sequence.audio_tracks[0].solo,
              "audio-mix: track muted/solo round-trip through serialization");
        check(loaded.sequence.audio_tracks[0].gain_db == -9.0f,
              "audio-mix: track gain round-trips through serialization");

        check(undo.undo(p.sequence), "audio-mix: undo gain");
        check(p.sequence.audio_tracks[0].gain_db == 0.0f,
              "audio-mix: undo restores the track gain default");
        check(undo.undo(p.sequence), "audio-mix: undo solo");
        check(p.sequence.audio_tracks[0].muted && !p.sequence.audio_tracks[0].solo,
              "audio-mix: undo solo keeps the mute");
        check(undo.undo(p.sequence), "audio-mix: undo mute");
        check(!p.sequence.audio_tracks[0].muted && !p.sequence.audio_tracks[0].solo,
              "audio-mix: undo restores track flags");
        check(undo.undo(p.sequence), "audio-mix: undo audio");
        check(p.sequence.audio_tracks[0].clips[0].volume_db == 0.0f &&
                  p.sequence.audio_tracks[0].clips[0].pan == 0.0f,
              "audio-mix: undo restores clip defaults");

        cmd = set_track_locked(p.sequence, Track::Kind::Audio, 0, true);
        check(cmd != nullptr, "audio-mix: set_track_locked returns command");
        undo.record(std::move(cmd));
        check(p.sequence.audio_tracks[0].locked, "audio-mix: lock applied");
        check(undo.undo(p.sequence), "audio-mix: undo lock");
        check(!p.sequence.audio_tracks[0].locked, "audio-mix: undo clears lock");
    }

    {
        // Visual transform + composite: set, undo, linked-mate inheritance, and
        // serialization round-trip for the per-clip visual/composite fields.
        Project p = make_project();
        UndoStack undo;
        Clip a;
        a.media = 0;
        a.tl_in = 0;
        a.src_in = 0;
        a.src_out = 30;
        auto cmd = place_clip(p.sequence, Track::Kind::Video, 0, a, Placement::Overwrite);
        check(cmd != nullptr, "visual: place video clip");
        undo.record(std::move(cmd));
        const ClipId id = p.sequence.video_tracks[0].clips[0].id;

        cmd = set_clip_transform(p.sequence, Track::Kind::Video, 0, id,
                                 1.5f, 1.0f, 120.0, -80.0, 45.0f, 10.0, -5.0, true, false);
        check(cmd != nullptr, "visual: set_clip_transform returns command");
        undo.record(std::move(cmd));
        const auto& vc0 = p.sequence.video_tracks[0].clips[0];
        check(vc0.scale_x == 1.5f && vc0.scale_y == 1.0f && vc0.pos_x == 120.0 &&
                  vc0.pos_y == -80.0 && vc0.rotation_deg == 45.0f &&
                  vc0.anchor_dx == 10.0 && vc0.anchor_dy == -5.0 &&
                  vc0.flip_h && !vc0.flip_v,
              "visual: clip carries full transform");

        cmd = set_clip_composite(p.sequence, Track::Kind::Video, 0, id, 0.5f, BlendMode::Screen);
        check(cmd != nullptr, "visual: set_clip_composite returns command");
        undo.record(std::move(cmd));
        check(p.sequence.video_tracks[0].clips[0].opacity == 0.5f &&
                  p.sequence.video_tracks[0].clips[0].blend_mode == BlendMode::Screen,
              "visual: clip carries composite opacity/blend");

        std::string err;
        check(save_project(p, "/tmp/opencode/media/visual.ehproj", &err), "visual: save project");
        Project loaded;
        check(load_project(loaded, "/tmp/opencode/media/visual.ehproj", &err), "visual: load project");
        const auto& lc = loaded.sequence.video_tracks[0].clips[0];
        check(lc.scale_x == 1.5f && lc.scale_y == 1.0f && lc.pos_x == 120.0 &&
                  lc.pos_y == -80.0 && lc.rotation_deg == 45.0f &&
                  lc.anchor_dx == 10.0 && lc.anchor_dy == -5.0 &&
                  lc.flip_h && !lc.flip_v && lc.opacity == 0.5f &&
                  lc.blend_mode == BlendMode::Screen,
              "visual: full transform/composite round-trip through serialization");

        check(undo.undo(p.sequence), "visual: undo composite");
        check(p.sequence.video_tracks[0].clips[0].opacity == 1.0f &&
                  p.sequence.video_tracks[0].clips[0].blend_mode == BlendMode::Normal,
              "visual: undo restores composite defaults");
        check(undo.undo(p.sequence), "visual: undo transform");
        const auto& vc_undone = p.sequence.video_tracks[0].clips[0];
        check(vc_undone.scale_x == 1.0f && vc_undone.scale_y == 1.0f &&
                  vc_undone.pos_x == 0.0 && vc_undone.pos_y == 0.0 &&
                  vc_undone.rotation_deg == 0.0f && vc_undone.anchor_dx == 0.0 &&
                  vc_undone.anchor_dy == 0.0 && !vc_undone.flip_h && !vc_undone.flip_v,
              "visual: undo restores transform defaults");

        // Linked audio mate inherits the video transform (shared fields).
        Project lp = make_project();
        Clip v;
        v.media = 0;
        v.tl_in = 0;
        v.src_in = 0;
        v.src_out = 30;
        cmd = place_clip(lp.sequence, Track::Kind::Video, 0, v, Placement::Overwrite);
        check(cmd != nullptr, "visual: place linked video");
        undo.record(std::move(cmd));
        Clip au;
        au.media = 0;
        au.tl_in = 0;
        au.src_in = 0;
        au.src_out = 30;
        cmd = place_clip(lp.sequence, Track::Kind::Audio, 0, au, Placement::Overwrite);
        check(cmd != nullptr, "visual: place linked audio");
        undo.record(std::move(cmd));
        const ClipId vid = lp.sequence.video_tracks[0].clips[0].id;
        cmd = link_clip(lp.sequence, Track::Kind::Video, 0, vid);
        check(cmd != nullptr, "visual: link video to audio mate");
        undo.record(std::move(cmd));
        cmd = set_clip_transform(lp.sequence, Track::Kind::Video, 0, vid,
                                 2.0f, 2.0f, 0.0, 0.0, 90.0f, 0.0, 0.0, false, true);
        check(cmd != nullptr, "visual: transform linked video");
        undo.record(std::move(cmd));
        check(lp.sequence.audio_tracks[0].clips[0].scale_x == 2.0f &&
                  lp.sequence.audio_tracks[0].clips[0].flip_v,
              "visual: linked audio mate inherits the video transform");
    }

    {
        // Deliver context persistence: the panel's expensive deliverables
        // settings + the render queue (staged jobs and finished cards with
        // their completion time) save inside the project file and come back
        // intact, so reopening a project hands you straight back the export.
        Project p = make_project();
        DeliverSettings ds;
        ds.video.codec = "H.265";
        ds.video.encoder = EncoderBackend::NVIDIA;
        ds.video.rate_control = RateControl::VBRTargetKbps;
        ds.video.target_bitrate_kbps = 45000;
        ds.video.max_bitrate_kbps = 50000;
        ds.video.custom_fps = 59.94;
        ds.video.resolution = "2560 x 1440";
        ds.audio.bitrate_kbps = 320;
        ds.file.file_name = "final_v2";
        ds.file.location = "/tmp/opencode/media";
        p.deliver_settings = ds;

        RenderJobSnapshot staged;
        staged.id = 7;
        staged.name = "final_v2";
        staged.settings = ds;
        staged.output_path = "/tmp/opencode/media/final_v2.mkv";
        staged.total_frames = 4500;
        staged.status = 0;  // Queued
        p.render_jobs.push_back(staged);

        RenderJobSnapshot finished;
        finished.id = 8;
        finished.name = "final_v1";
        finished.settings = ds;
        finished.output_path = "/tmp/opencode/media/final_v1.mkv";
        finished.total_frames = 4500;
        finished.status = 2;  // Completed
        finished.progress = 1.0;
        finished.elapsed_seconds = 42.5;
        finished.frames_rendered = 4500;
        finished.finished_at = "14:22:03";
        p.render_jobs.push_back(finished);

        RenderJobSnapshot failed;
        failed.id = 9;
        failed.name = "wrong_codec";
        failed.settings = ds;
        failed.status = 3;  // Failed
        failed.error = "encoder init failed";
        failed.finished_at = "14:40:11";
        p.render_jobs.push_back(failed);

        RenderJobSnapshot mid_render;
        mid_render.id = 10;
        mid_render.name = "in_flight";
        mid_render.settings = ds;
        mid_render.status = 1;  // Rendering — must come back Queued
        mid_render.progress = 0.4;
        p.render_jobs.push_back(mid_render);

        std::string derr;
        check(save_project(p, "/tmp/opencode/media/deliver.ehproj", &derr),
              "deliver: save project with deliver settings + queue");
        Project loaded;
        check(load_project(loaded, "/tmp/opencode/media/deliver.ehproj", &derr),
              "deliver: load project");

        const auto& ld = loaded.deliver_settings;
        check(ld.video.codec == "H.265" && ld.video.encoder == EncoderBackend::NVIDIA &&
                  ld.video.rate_control == RateControl::VBRTargetKbps &&
                  ld.video.target_bitrate_kbps == 45000 &&
                  ld.video.max_bitrate_kbps == 50000 &&
                  ld.video.resolution == "2560 x 1440",
              "deliver: video settings round-trip");
        check(ld.audio.bitrate_kbps == 320 && ld.file.file_name == "final_v2" &&
                  ld.file.location == "/tmp/opencode/media",
              "deliver: audio/file settings round-trip");
        check(loaded.render_jobs.size() == 4, "deliver: all four jobs round-trip");
        if (loaded.render_jobs.size() == 4) {
            check(loaded.render_jobs[0].id == 7 && loaded.render_jobs[0].status == 0 &&
                      loaded.render_jobs[0].output_path == "/tmp/opencode/media/final_v2.mkv" &&
                      loaded.render_jobs[0].settings.video.target_bitrate_kbps == 45000,
                  "deliver: queued job round-trips with settings");
            check(loaded.render_jobs[1].status == 2 &&
                      loaded.render_jobs[1].finished_at == "14:22:03" &&
                      loaded.render_jobs[1].frames_rendered == 4500,
                  "deliver: finished card keeps its completion time");
            check(loaded.render_jobs[2].status == 3 &&
                      loaded.render_jobs[2].error == "encoder init failed",
                  "deliver: failed card round-trips its error");
            // A job saved mid-render must not resurrect as Rendering.
            const RenderJob resumed = render_job_from_snapshot(loaded.render_jobs[3]);
            check(resumed.status == RenderJob::Status::Queued &&
                      resumed.settings.video.codec == "H.265",
                  "deliver: mid-render snapshot restores as Queued");
        }

        const RenderJobSnapshot snap = render_job_snapshot([&] {
            RenderJob j;
            j.name = "snapshot_check";
            j.status = RenderJob::Status::Completed;
            j.finished_at = "15:00:00";
            return j;
        }());
        check(snap.finished_at == "15:00:00" && snap.status == 2,
              "deliver: render_job_snapshot maps all fields");
    }

    {
        // Clip trim/extend edit ops. Head/tail trims move the source window with
        // the edge; the tail can grow back into the deleted region after a
        // blade+delete ("regrow"), and clamps keep edges off neighbors and within
        // the media duration.
        Project p = make_project();
        Clip a;
        a.media = 0;
        a.name = "A";
        a.tl_in = 0;
        a.src_in = 0;
        a.src_out = 30;
        auto cmd = place_clip(p.sequence, Track::Kind::Video, 0, a, Placement::Overwrite);
        check(cmd != nullptr, "trim: place clip");
        undo.record(std::move(cmd));
        const ClipId id = p.sequence.video_tracks[0].clips[0].id;

        // Regrow the tail: media has 300 frames, so the clip can extend to 300.
        cmd = trim_clip_tail(p.sequence, Track::Kind::Video, 0, id, 60, 300);
        check(cmd != nullptr, "trim: tail regrow returns command");
        undo.record(std::move(cmd));
        check(p.sequence.video_tracks[0].clips[0].tl_out == 60 &&
                  p.sequence.video_tracks[0].clips[0].src_out == 60,
              "trim: tail regrow extends tl_out and src_out together");

        // Request beyond the media duration clamps to the media end.
        cmd = trim_clip_tail(p.sequence, Track::Kind::Video, 0, id, 10000, 300);
        check(cmd != nullptr, "trim: over-long tail request returns command");
        undo.record(std::move(cmd));
        check(p.sequence.video_tracks[0].clips[0].tl_out == 300 &&
                  p.sequence.video_tracks[0].clips[0].src_out == 300,
              "trim: tail clamps to the media duration");

        // A right neighbor blocks further tail growth. First shrink the clip so
        // its source window has room left beyond the neighbor (otherwise the
        // media-duration clamp, not the neighbor, bounds the trim).
        cmd = trim_clip_tail(p.sequence, Track::Kind::Video, 0, id, 200, 300);
        check(cmd != nullptr, "trim: shrink tail returns command");
        undo.record(std::move(cmd));
        check(p.sequence.video_tracks[0].clips[0].tl_out == 200 &&
                  p.sequence.video_tracks[0].clips[0].src_out == 200,
              "trim: tail shrink moves src_out with the edge");
        Clip b;
        b.media = 0;
        b.name = "B";
        b.tl_in = 210;
        b.src_in = 0;
        b.src_out = 30;
        cmd = place_clip(p.sequence, Track::Kind::Video, 0, b, Placement::Overwrite);
        check(cmd != nullptr, "trim: place right neighbor");
        undo.record(std::move(cmd));
        cmd = trim_clip_tail(p.sequence, Track::Kind::Video, 0, id, 10000, 300);
        check(cmd != nullptr, "trim: neighbor-blocked tail returns command");
        undo.record(std::move(cmd));
        check(p.sequence.video_tracks[0].clips[0].tl_out == 210,
              "trim: tail clamps to the right neighbor's start");

        // Head extension (regrow left) pulls src_in with the edge.
        cmd = trim_clip_head(p.sequence, Track::Kind::Video, 0, id, 20, 300);
        check(cmd != nullptr, "trim: head regrow returns command");
        undo.record(std::move(cmd));
        check(p.sequence.video_tracks[0].clips[0].tl_in == 20 &&
                  p.sequence.video_tracks[0].clips[0].src_in == 20,
              "trim: head regrow extends tl_in and src_in together");

        // ...but only as far back as the source allows (src_in >= 0).
        cmd = trim_clip_head(p.sequence, Track::Kind::Video, 0, id, -500, 300);
        check(cmd != nullptr, "trim: over-long head request returns command");
        undo.record(std::move(cmd));
        check(p.sequence.video_tracks[0].clips[0].tl_in == 0 &&
                  p.sequence.video_tracks[0].clips[0].src_in == 0,
              "trim: head clamps to the source start");

        // No-op edge (same position) yields no command.
        cmd = trim_clip_tail(p.sequence, Track::Kind::Video, 0, id, 210, 300);
        check(cmd == nullptr, "trim: same-position tail is a no-op");

        // Undo unwinds the trim history back to the original [0,30).
        check(undo.undo(p.sequence), "trim: undo over-long head");
        check(p.sequence.video_tracks[0].clips[0].tl_in == 20, "trim: undo restores head");
        check(undo.undo(p.sequence), "trim: undo head regrow");
        check(undo.undo(p.sequence), "trim: undo neighbor-blocked tail");
        check(undo.undo(p.sequence), "trim: undo place neighbor");
        check(undo.undo(p.sequence), "trim: undo shrink tail");
        check(undo.undo(p.sequence), "trim: undo over-long tail");
        check(undo.undo(p.sequence), "trim: undo tail regrow");
        check(p.sequence.video_tracks[0].clips[0].tl_out == 30 &&
                  p.sequence.video_tracks[0].clips[0].src_out == 30,
              "trim: undo full history restores the original [0,30)");

        // Locked tracks reject trims.
        Track& v1 = p.sequence.video_tracks[0];
        v1.locked = true;
        cmd = trim_clip_tail(p.sequence, Track::Kind::Video, 0, id, 60, 300);
        check(cmd == nullptr, "trim: locked track returns nullptr");
        v1.locked = false;
    }

    {
        // Linked A/V pair: trimming one edge moves the mate by the same delta,
        // and the pair's shared limit is the intersection of both clips' ranges.
        Project p = make_project();
        Clip v;
        v.media = 0;
        v.name = "V";
        v.tl_in = 30;
        v.src_in = 30;
        v.src_out = 60;
        Clip au = v;
        auto cmd = place_linked_clip(p.sequence, 0, 0, v, au, Placement::Overwrite);
        check(cmd != nullptr, "trim-linked: place linked pair");
        undo.record(std::move(cmd));
        const ClipId vid = p.sequence.video_tracks[0].clips[0].id;

        // An audio clip sitting right after the mate's audio space limits the pair.
        Clip constrict;
        constrict.media = 0;
        constrict.name = "CONSTRICT";
        constrict.tl_in = 70;
        constrict.src_in = 0;
        constrict.src_out = 10;
        cmd = place_clip(p.sequence, Track::Kind::Audio, 0, constrict, Placement::Overwrite);
        check(cmd != nullptr, "trim-linked: place constricting audio clip");
        undo.record(std::move(cmd));

        // Video alone could reach 300, but the audio mate stops at its neighbor
        // (tl_in 70), so both clips stop at 70.
        cmd = trim_clip_tail(p.sequence, Track::Kind::Video, 0, vid, 200, 300);
        check(cmd != nullptr, "trim-linked: mate-constrained tail returns command");
        undo.record(std::move(cmd));
        check(p.sequence.video_tracks[0].clips[0].tl_out == 70 &&
                  p.sequence.audio_tracks[0].clips[0].tl_out == 70,
              "trim-linked: pair tail clamps to the mate's neighbor");
        check(p.sequence.video_tracks[0].clips[0].src_out == 70 &&
                  p.sequence.audio_tracks[0].clips[0].src_out == 70,
              "trim-linked: pair src_out follows in lockstep");

        // A mate-constrained HEAD trim behaves symmetrically: the audio mate ends
        // up pinned at 20 by its left neighbor (which ends at 20), so the video
        // cannot go past it.
        Clip head_limit;
        head_limit.media = 0;
        head_limit.name = "HEADLIMIT";
        head_limit.tl_in = 10;
        head_limit.src_in = 0;
        head_limit.src_out = 10;
        cmd = place_clip(p.sequence, Track::Kind::Audio, 0, head_limit, Placement::Overwrite);
        check(cmd != nullptr, "trim-linked: place head-limiting audio clip");
        undo.record(std::move(cmd));
        cmd = trim_clip_head(p.sequence, Track::Kind::Video, 0, vid, -500, 300);
        check(cmd != nullptr, "trim-linked: mate-constrained head returns command");
        undo.record(std::move(cmd));
        const Clip& mate = *p.sequence.audio_tracks[0].clip_with_id(
            p.sequence.video_tracks[0].clips[0].linked_id);
        check(p.sequence.video_tracks[0].clips[0].tl_in == 20 &&
                  p.sequence.video_tracks[0].clips[0].src_in == 20 &&
                  mate.tl_in == 20 && mate.src_in == 20,
              "trim-linked: pair head clamps to the audio mate's neighbor");
    }

    // Batch-capable atomic group move.
    {
        Project p = make_project();
        Clip a;
        a.media = 0;
        a.name = "BATH_A";
        a.tl_in = 0;
        a.src_in = 0;
        a.src_out = 30;
        Clip b;
        b.media = 0;
        b.name = "BATH_B";
        b.tl_in = 30;
        b.src_in = 0;
        b.src_out = 30;
        auto cmd = place_clip(p.sequence, Track::Kind::Video, 0, a, Placement::Overwrite);
        check(cmd != nullptr, "batch-move: place A");
        undo.record(std::move(cmd));
        cmd = place_clip(p.sequence, Track::Kind::Video, 0, b, Placement::Overwrite);
        check(cmd != nullptr, "batch-move: place B");
        undo.record(std::move(cmd));
        const ClipId ia = p.sequence.video_tracks[0].clips[0].id;
        const ClipId ib = p.sequence.video_tracks[0].clips[1].id;

        // Moving the pair +40 as one atomic op must NOT let the first placement
        // trim the second (sequential move_clip would shrink B to [120,160] until
        // its own rep ran — or outright delete it when fully covered).
        cmd = move_clips_batch(p.sequence, {{ia, Track::Kind::Video, 0, 40},
                                            {ib, Track::Kind::Video, 0, 70}});
        check(cmd != nullptr, "batch-move: atomic move returns command");
        undo.record(std::move(cmd));
        const Clip* ca = p.sequence.video_tracks[0].clip_with_id(ia);
        const Clip* cb = p.sequence.video_tracks[0].clip_with_id(ib);
        check(ca && cb, "batch-move: both clips survive the commit");
        check(ca && ca->tl_in == 40 && ca->tl_out == 70 && ca->duration() == 30,
              "batch-move: A keeps its full size at the target");
        check(cb && cb->tl_in == 70 && cb->tl_out == 100 && cb->duration() == 30,
              "batch-move: B keeps its full size and spacing after A");
        check(p.sequence.video_tracks[0].clips.size() == 2,
              "batch-move: no clip is lost or duplicated");

        // A stationary clip partially overlapped by the final group is still
        // overwritten at the boundary (intended) — and ONLY the stationary clip
        // loses frames; the dragged group keeps its exact size.
        Clip c;
        c.media = 0;
        c.name = "BATH_C";
        c.tl_in = 130;
        c.src_in = 0;
        c.src_out = 90;
        cmd = place_clip(p.sequence, Track::Kind::Video, 0, c, Placement::Overwrite);
        check(cmd != nullptr, "batch-move: place stationary C");
        undo.record(std::move(cmd));
        const ClipId ic = [&] {
            for (const auto& cl : p.sequence.video_tracks[0].clips)
                if (cl.name == "BATH_C") return cl.id;
            return ClipId{0};
        }();
        check(ic != 0, "batch-move: C is on the timeline");
        cmd = move_clips_batch(p.sequence, {{ia, Track::Kind::Video, 0, 120},
                                            {ib, Track::Kind::Video, 0, 150}});
        check(cmd != nullptr, "batch-move: overlap-into-stationary returns command");
        undo.record(std::move(cmd));
        const Clip* cc = p.sequence.video_tracks[0].clip_with_id(ic);
        const Clip* ca2 = p.sequence.video_tracks[0].clip_with_id(ia);
        const Clip* cb2 = p.sequence.video_tracks[0].clip_with_id(ib);
        check(cc && cc->tl_in == 180 && cc->duration() == 40,
              "batch-move: stationary clip is trimmed only at the overwrite boundary");
        check(ca2 && cb2 && ca2->tl_in == 120 && ca2->duration() == 30 && cb2->duration() == 30,
              "batch-move: dragged clips keep their exact size and relative spacing");
    }

    if (failures == 0) {
        std::printf("ALL TESTS PASSED\n");
        return 0;
    }
    std::fprintf(stderr, "%d TEST(S) FAILED\n", failures);
    return 1;
}
