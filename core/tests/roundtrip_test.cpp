#include "canvas/core/media/audio_waveform.hpp"
#include "canvas/core/media/audio_decoder.hpp"
#include "canvas/core/media/video_decoder.hpp"
#include "canvas/core/export/renderer.hpp"
#include "canvas/core/export/render_queue.hpp"
#include "canvas/core/project/project.hpp"
#include "canvas/core/timeline/audio_fade.hpp"
#include "canvas/core/timeline/edit_ops.hpp"
#include "canvas/core/timeline/title.hpp"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

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

float test_wav_sample(int64_t frame, int ch) {
    const double ph = kTau * (300.0 + 100.0 * ch) / 48000.0 * static_cast<double>(frame) +
                      (ch ? 1.7 : 0.0);
    const float v = static_cast<float>(0.8 * std::sin(ph));
    return static_cast<float>(std::llround(v * 32767.0f)) / 32768.0f;
}
}

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

}

int main() {
    UndoStack undo;

    {
        Project p = make_project();

        Clip a;
        a.media = 0;
        a.name = "A";
        a.tl_in = 0;
        a.src_in = 0;
        a.src_out = 60;
        auto cmd = place_clip(p.sequence, Track::Kind::Video, 0, a, Placement::Overwrite);
        check(cmd != nullptr, "place_clip overwrite returns command");
        undo.record(std::move(cmd));

        Clip b;
        b.media = 0;
        b.name = "B";
        b.src_in = 0;
        b.src_out = 90;
        cmd = place_clip(p.sequence, Track::Kind::Video, 0, b, Placement::AppendAtEnd);
        check(cmd != nullptr, "append_clip returns command");
        undo.record(std::move(cmd));

        check(p.sequence.duration_frames() == 150, "duration after two clips is 150");

        cmd = blade_at(p.sequence, Track::Kind::Video, 0, 30);
        check(cmd != nullptr, "blade_at returns command");
        undo.record(std::move(cmd));
        check(p.sequence.video_tracks[0].clips.size() == 3, "blade splits clip into two (3 clips)");

        cmd = lift_range(p.sequence, Track::Kind::Video, 0, 0, 30);
        check(cmd != nullptr, "lift_range returns command");
        undo.record(std::move(cmd));
        check(p.sequence.video_tracks[0].clips.size() == 2, "lift removes one clip");
        const int64_t dur_before = p.sequence.duration_frames();
        check(dur_before == 150, "lift keeps total duration (gap left)");

        check(undo.undo(p.sequence), "undo lift");
        check(p.sequence.video_tracks[0].clips.size() == 3, "after undo, 3 clips again");

        check(undo.redo(p.sequence), "redo lift");
        check(p.sequence.video_tracks[0].clips.size() == 2, "after redo, 2 clips");

        cmd = ripple_delete_range(p.sequence, Track::Kind::Video, 0, 30, 120);
        check(cmd != nullptr, "ripple_delete_range returns command");
        undo.record(std::move(cmd));
        std::printf("  info: after ripple delete, duration=%lld clips=%zu\n",
                    (long long)p.sequence.duration_frames(), p.sequence.video_tracks[0].clips.size());
        check(p.sequence.video_tracks[0].clips.size() == 1, "ripple delete leaves one clip");
        check(p.sequence.video_tracks[0].clips[0].duration() == 30, "ripple delete leaves 30-frame clip");
        check(p.sequence.duration_frames() == 60, "ripple delete closes gap (end frame 60)");

        p.sequence.video_tracks[0].clips[0].clip_tag = Clip::ClipTag::GoodTake;
        p.sequence.video_tracks[0].clips[0].clip_color = 7;
        p.sequence.video_tracks[0].clips[0].comments = "keeper shot";
        p.sequence.video_tracks[0].clips[0].speed_enabled = true;
        p.sequence.video_tracks[0].clips[0].speed_factor = 2.0;
        p.sequence.video_tracks[0].clips[0].pitch_semitones = 2.0f;
        p.sequence.video_tracks[0].clips[0].pitch_cents = 50.0f;
        p.sequence.video_tracks[0].clips[0].eq_enabled = true;
        auto& eq = p.sequence.video_tracks[0].clips[0].eq_bands;
        eq[1].gain = -4.5f;
        eq[1].q = 2.0f;
        eq[3].frequency = 4000.0f;
        eq[4].enabled = false;

        std::string err;
        check(save_project(p, "/tmp/opencode/media/roundtrip.ehproj", &err), "save_project");

        Project loaded;
        check(load_project(loaded, "/tmp/opencode/media/roundtrip.ehproj", &err), "load_project");
        check(tracks_equal(p.sequence.video_tracks, loaded.sequence.video_tracks),
              "video tracks identical after round-trip");
        const Clip& lc = loaded.sequence.video_tracks[0].clips[0];
        check(lc.clip_tag == Clip::ClipTag::GoodTake, "clip tag preserved");
        check(lc.clip_color == 7, "clip colour preserved");
        check(lc.comments == "keeper shot", "clip comments preserved");
        check(lc.speed_enabled && lc.speed_factor == 2.0, "speed fields preserved");
        check(lc.pitch_semitones == 2.0f && lc.pitch_cents == 50.0f, "pitch fields preserved");
        check(lc.eq_enabled, "eq_enabled preserved");
        check(lc.eq_bands[1].gain == -4.5f && lc.eq_bands[1].q == 2.0f,
              "eq band gain/q preserved");
        check(lc.eq_bands[3].frequency == 4000.0f, "eq band frequency preserved");
        check(!lc.eq_bands[4].enabled, "eq band disabled flag preserved");
        check(lc.eq_bands[4].type == Clip::default_eq_bands()[4].type, "eq band type preserved");
        check(p.media.size() == loaded.media.size(), "media registry preserved");
        check(loaded.media.size() == 1 && loaded.media[0].bin == "Scratch", "media bin preserved");
        check(loaded.bins.size() == 1 && loaded.bins[0] == "Scratch", "bins list preserved");

        check(undo.undo(p.sequence), "undo ripple delete");
        check(p.sequence.video_tracks[0].clips.size() == 2, "after undo ripple, 2 clips");
        check(p.sequence.duration_frames() == 150, "after undo ripple, duration 150");

        while (undo.undo(p.sequence)) {}
        check(p.sequence.video_tracks[0].clips.empty(), "unwound to empty track");
    }

    {
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
            check(!clips[0].has_transition_out(), "transition-blade: seam is a plain cut (left half)");
            check(!clips[1].has_transition_in(), "transition-blade: seam is a plain cut (right half)");
            check(clips[1].has_transition_out() && clips[1].transition_out_duration == 14,
                  "transition-blade: fade stays on the original tail");
        }
    }

    {
        Project p = make_project();
        Clip a;
        a.media = 0;
        a.name = "Rate2";
        a.tl_in = 828;
        a.src_in = 0;
        a.src_out = 30661;
        auto cmd = place_clip(p.sequence, Track::Kind::Video, 0, a, Placement::Overwrite, 60.0);
        check(cmd != nullptr, "rate-blade: place 60fps clip at 30fps timeline");
        undo.record(std::move(cmd));

        const auto& placed = p.sequence.video_tracks[0].clips[0];
        const double tl_span = static_cast<double>(placed.tl_out - placed.tl_in);
        check(tl_span > 0.0 && std::abs((placed.src_out - placed.src_in) / tl_span - 2.0) < 1e-3,
              "rate-blade: placed clip has src/tl rate ~2.0");

        cmd = blade_at(p.sequence, Track::Kind::Video, 0, 1214);
        check(cmd != nullptr, "rate-blade: cut mid-rate clip");
        undo.record(std::move(cmd));

        const auto& clips = p.sequence.video_tracks[0].clips;
        if (clips.size() == 2) {
            const int64_t left_src_out = clips[0].src_out;
            check(left_src_out == 772,
                  "rate-blade: left half src_out picks rate-aware frame (772), not naive 386");
            const int64_t right_src_in = clips[1].src_in;
            check(right_src_in == left_src_out,
                  "rate-blade: right half src_in == left half src_out (contiguous)");
            const double lrate = static_cast<double>(clips[0].src_out - clips[0].src_in) /
                                 static_cast<double>(clips[0].tl_out - clips[0].tl_in);
            const double rrate = static_cast<double>(clips[1].src_out - clips[1].src_in) /
                                 static_cast<double>(clips[1].tl_out - clips[1].tl_in);
            check(std::abs(lrate - 2.0) < 1e-3,
                  "rate-blade: left half keeps rate 2.0 (not collapsed to 1.0)");
            check(std::abs(rrate - 2.0) < 1e-3, "rate-blade: right half keeps rate 2.0");
        } else {
            check(false, "rate-blade: clip split in two");
        }
    }

    {
        Project p = make_project();
        Clip v;
        v.media = 0;
        v.name = "V60";
        v.tl_in = 0;
        v.src_in = 0;
        v.src_out = 600;
        Clip a = v;
        a.name = "A60";
        auto cmd = place_linked_clip(p.sequence, 0, 0, v, a, Placement::Overwrite, 60.0);
        check(cmd != nullptr, "linked-rate-blade: place linked 60fps pair");
        undo.record(std::move(cmd));

        cmd = blade_linked_at(p.sequence, Track::Kind::Video, 0, 150);
        check(cmd != nullptr, "linked-rate-blade: cut linked pair");
        undo.record(std::move(cmd));

        const auto& vc = p.sequence.video_tracks[0].clips;
        const auto& ac = p.sequence.audio_tracks[0].clips;
        if (vc.size() == 2 && ac.size() == 2) {
            auto rate = [](const Clip& c) {
                return static_cast<double>(c.src_out - c.src_in) /
                       static_cast<double>(c.tl_out - c.tl_in);
            };
            check(std::abs(rate(vc[0]) - 2.0) < 1e-3 && std::abs(rate(vc[1]) - 2.0) < 1e-3,
                  "linked-rate-blade: both video halves keep rate 2.0");
            check(std::abs(rate(ac[0]) - 2.0) < 1e-3 && std::abs(rate(ac[1]) - 2.0) < 1e-3,
                  "linked-rate-blade: both audio halves keep rate 2.0");
        } else {
            check(false, "linked-rate-blade: linked pair split in two on both tracks");
        }
    }

    {
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

        cmd = move_clip(p.sequence, Track::Kind::Video, 0, vc.id,
                        Track::Kind::Video, 0, 30);
        check(cmd != nullptr, "move linked video returns command");
        undo.record(std::move(cmd));
        check(p.sequence.video_tracks[0].clips[0].tl_in == 30 &&
              p.sequence.audio_tracks[0].clips[0].tl_in == 30,
              "moving video moves audio mate");

        const auto& mv = p.sequence.video_tracks[0].clips[0];
        cmd = lift_range(p.sequence, Track::Kind::Video, 0, mv.tl_in, mv.tl_out);
        check(cmd != nullptr, "lift linked video returns command");
        undo.record(std::move(cmd));
        check(p.sequence.video_tracks[0].clips.empty() && p.sequence.audio_tracks[0].clips.empty(),
              "deleting video removes linked audio");

        undo.undo(p.sequence);
        undo.undo(p.sequence);
        const auto& uvc = p.sequence.video_tracks[0].clips[0];
        cmd = unlink_clip(p.sequence, Track::Kind::Video, 0, uvc.id);
        check(cmd != nullptr, "unlink returns command");
        undo.record(std::move(cmd));
        check(!p.sequence.video_tracks[0].clips[0].is_linked() &&
              !p.sequence.audio_tracks[0].clips[0].is_linked(),
              "unlink clears both sides");

        const auto& uc = p.sequence.video_tracks[0].clips[0];
        cmd = move_clip(p.sequence, Track::Kind::Video, 0, uc.id, Track::Kind::Video, 0, 40);
        check(cmd != nullptr, "move unlinked video returns command");
        undo.record(std::move(cmd));
        check(p.sequence.video_tracks[0].clips[0].tl_in == 40 &&
              p.sequence.audio_tracks[0].clips[0].tl_in == 0,
              "unlinked video moves independently of audio");

        const auto& relink = p.sequence.video_tracks[0].clips[0];
        cmd = link_clip(p.sequence, Track::Kind::Video, 0, relink.id);
        check(cmd != nullptr, "link_clip returns command");
        undo.record(std::move(cmd));
        check(p.sequence.video_tracks[0].clips[0].is_linked() &&
              p.sequence.audio_tracks[0].clips[0].is_linked(),
              "link_clip re-links both sides");

        cmd = link_clip(p.sequence, Track::Kind::Video, 0, relink.id);
        check(cmd == nullptr, "link_clip on linked clip returns nullptr");
    }

    {
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

        float last_peak = 0.0f;
        for (std::size_t i = 0; i < raw.buckets; ++i)
            last_peak = std::max(last_peak, raw.peak[i]);
        check(out.peak.back() >= last_peak - 1e-6f,
              "reduce_waveform peak reflects max of source range");

        AudioWaveform empty = reduce_waveform(AudioWaveform{}, 64);
        check(empty.buckets == 64 && empty.peak.size() == 64,
              "reduce_waveform handles empty source safely");
        AudioWaveform zero = reduce_waveform(raw, 0);
        check(zero.buckets == 0 && zero.peak.empty(), "reduce_waveform handles 0 target");

        const double mid = raw.peak[raw.buckets / 2];
        AudioWaveform lo = reduce_waveform(raw, 8, 0.0, 0.5);
        AudioWaveform hi = reduce_waveform(raw, 8, 0.5, 1.0);
        check(lo.buckets == 8 && hi.buckets == 8,
              "reduce_waveform range yields requested bucket count");
        check(lo.peak.back() <= mid + 1e-6f && lo.peak.back() >= lo.peak.front(),
              "reduce_waveform lo-half stays within its half and rises");
        check(hi.peak.front() >= mid - 1e-6f,
              "reduce_waveform hi-half begins at the midline peak");
        check(hi.peak.back() >= raw.peak.back() - 1e-6f,
              "reduce_waveform hi-half ends at the source maximum");
        AudioWaveform whole = reduce_waveform(raw, 8, 0.0, 1.0);
        check(whole.peak.back() >= lo.peak.back() && whole.peak.front() <= hi.peak.front(),
              "reduce_waveform whole-file spans the range pieces");
        AudioWaveform bad = reduce_waveform(raw, 4, 0.5, 0.5);
        check(bad.peak[0] >= raw.peak[0], "reduce_waveform zero-width range is safe");

        AudioWaveform all = reduce_waveform(raw, 128, 0.0, 1.0);
        check(all.peak == out.peak && all.rms == out.rms,
              "reduce_waveform 3-arg full range matches 2-arg");
    }

    {
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
        AudioDecoder adec;
        std::string aerr;
        const char* apath = "/tmp/opencode/test_av.mp4";
        if (adec.open(apath) && adec.has_audio()) {
            const int out_rate = 44100;
            const int64_t base = static_cast<int64_t>(out_rate) * 20LL;
            auto a0 = adec.decode(base, 800, out_rate);
            check(a0 && !a0->samples.empty(), "audio decoder far seek returns frames");
            if (a0) {
                float peak = 0.0f;
                for (const float s : a0->samples) {
                    const float a = s < 0.0f ? -s : s;
                    if (a > peak) peak = a;
                }
                check(peak > 1e-4f,
                      "audio decoder far seek yields non-silent samples");
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
        const char* wpath = "/tmp/canvas_audio_fw_test.wav";
        const bool wrote = write_test_wav(wpath, 48000, 2, 6);
        check(wrote, "fw-jump: write test wav");
        if (!wrote) return 1;
        constexpr int kRate = 48000;
        constexpr int64_t kHead = 51200;
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
                if (std::fabs(s[i] - test_wav_sample(start_frame + i / 2, i % 2)) > 1e-3f)
                    return false;
            }
            return true;
        };
        {
            AudioDecoder a;
            check(a.open(wpath) && a.has_audio(), "fw-jump: open decoder A");
            auto c1 = a.decode(kHead, kSpan, kRate);
            check(c1 && c1->samples.size() >= static_cast<std::size_t>(kSpan) * 2,
                  "fw-jump: fresh first call at the head serves frames");
            check(region_matches(c1 ? c1->samples : std::vector<float>{}, kHead),
                  "fw-jump: fresh first call serves the requested region, not file start");
        }
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

        check(undo.undo(p.sequence), "undo transition");
        check(p.sequence.video_tracks[0].clips[0].transition_out == TransitionType::None,
              "undo clears transition");

        check(undo.redo(p.sequence), "redo transition");
        cmd = clear_clip_transition(p.sequence, Track::Kind::Video, 0, id_a);
        check(cmd != nullptr, "clear_clip_transition returns command");
        undo.record(std::move(cmd));
        check(p.sequence.video_tracks[0].clips[0].transition_out == TransitionType::None,
              "clear removes transition");
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
                    const float want = test_wav_sample(base + s / 2, s % 2) * g;
                    if (std::fabs(c->samples[s] - want) > 1e-3f) ok = false;
                }
            }
            check(ok, what);
        };
        probe(20, 4, "render-fade: audio before the fade window is unchanged");
        probe(25, 10, "render-fade: ConstantGain out-fade scales the exported mix");
        probe(25, 1, "render-fade: fade window start is attenuated");
        std::remove(wpath);
    }

    {
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

        check(undo.undo(p.sequence), "undo through edit");
        check(p.sequence.video_tracks[0].clips.size() == 2,
              "undo restores the two clips separated by a cut");
        check(undo.redo(p.sequence), "redo through edit");
        check(p.sequence.video_tracks[0].clips.size() == 1,
              "redo re-merges into one clip");

        Project q = make_project();
        Clip c2;
        c2.media = 0;
        c2.tl_in = 0;
        c2.src_in = 0;
        c2.src_out = 30;
        cmd = place_clip(q.sequence, Track::Kind::Video, 0, c2, Placement::Overwrite);
        undo.record(std::move(cmd));
        Clip d2;
        d2.media = 0;
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

        const auto& atrk = p.sequence.audio_tracks[0];
        const auto& vtrk = p.sequence.video_tracks[0];
        check(!atrk.collapsed && !vtrk.collapsed,
              "audio-mix: tracks start expanded");
        cmd = set_track_collapsed(p.sequence, Track::Kind::Audio, 0, true);
        check(cmd != nullptr, "collapse: set_track_collapsed returns command");
        undo.record(std::move(cmd));
        check(p.sequence.audio_tracks[0].collapsed, "collapse: audio track collapsed");
        cmd = set_track_collapsed(p.sequence, Track::Kind::Video, 0, true);
        check(cmd != nullptr, "collapse: set_track_collapsed (video) returns command");
        undo.record(std::move(cmd));
        cmd = set_track_collapsed(p.sequence, Track::Kind::Video, 0, true);
        check(cmd != nullptr && p.sequence.video_tracks[0].collapsed,
              "collapse: video track collapsed");
        undo.record(std::move(cmd));

        std::string cerr;
        check(save_project(p, "/tmp/opencode/media/collapse.ehproj", &cerr),
              "collapse: save project");
        Project cloaded;
        check(load_project(cloaded, "/tmp/opencode/media/collapse.ehproj", &cerr),
              "collapse: load project");
        check(cloaded.sequence.audio_tracks[0].collapsed &&
                  cloaded.sequence.video_tracks[0].collapsed,
              "collapse: collapsed flags round-trip through serialization");

        check(undo.undo(p.sequence), "collapse: undo video no-op");
        check(p.sequence.video_tracks[0].collapsed, "collapse: no-op undo leaves video collapsed");
        check(undo.undo(p.sequence), "collapse: undo video collapse");
        check(!p.sequence.video_tracks[0].collapsed, "collapse: undo expands video");
        check(undo.undo(p.sequence), "collapse: undo audio collapse");
        check(p.sequence.audio_tracks[0].collapsed == false &&
                  p.sequence.video_tracks[0].collapsed == false,
              "collapse: undo restores all-expanded state");

        check(!atrk.collapsed && !vtrk.collapsed,
              "collapse-all: tracks start expanded");
        cmd = set_all_tracks_collapsed(p.sequence, true);
        check(cmd != nullptr, "collapse-all: returns single command");
        undo.record(std::move(cmd));
        check(p.sequence.audio_tracks[0].collapsed && p.sequence.video_tracks[0].collapsed,
              "collapse-all: every track collapsed to a strip");
        check(undo.undo(p.sequence), "collapse-all: undo");
        check(!p.sequence.audio_tracks[0].collapsed &&
                  !p.sequence.video_tracks[0].collapsed,
              "collapse-all: one undo restores every track");
        cmd = set_all_tracks_collapsed(p.sequence, false);
        check(cmd == nullptr || (!p.sequence.audio_tracks[0].collapsed &&
                                    !p.sequence.video_tracks[0].collapsed),
              "collapse-all: expand-no-op leaves tracks expanded");

        try {
            Sequence empty;
            check(set_all_tracks_collapsed(empty, true) == nullptr,
                  "collapse-all: empty sequence returns nullptr");
        } catch (const std::exception&) {
            check(false, "collapse-all: empty sequence must not throw");
        }
    }

    {
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
        staged.status = 0;
        p.render_jobs.push_back(staged);

        RenderJobSnapshot finished;
        finished.id = 8;
        finished.name = "final_v1";
        finished.settings = ds;
        finished.output_path = "/tmp/opencode/media/final_v1.mkv";
        finished.total_frames = 4500;
        finished.status = 2;
        finished.progress = 1.0;
        finished.elapsed_seconds = 42.5;
        finished.frames_rendered = 4500;
        finished.finished_at = "14:22:03";
        p.render_jobs.push_back(finished);

        RenderJobSnapshot failed;
        failed.id = 9;
        failed.name = "wrong_codec";
        failed.settings = ds;
        failed.status = 3;
        failed.error = "encoder init failed";
        failed.finished_at = "14:40:11";
        p.render_jobs.push_back(failed);

        RenderJobSnapshot mid_render;
        mid_render.id = 10;
        mid_render.name = "in_flight";
        mid_render.settings = ds;
        mid_render.status = 1;
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

        cmd = trim_clip_tail(p.sequence, Track::Kind::Video, 0, id, 60, 300);
        check(cmd != nullptr, "trim: tail regrow returns command");
        undo.record(std::move(cmd));
        check(p.sequence.video_tracks[0].clips[0].tl_out == 60 &&
                  p.sequence.video_tracks[0].clips[0].src_out == 60,
              "trim: tail regrow extends tl_out and src_out together");

        cmd = trim_clip_tail(p.sequence, Track::Kind::Video, 0, id, 10000, 300);
        check(cmd != nullptr, "trim: over-long tail request returns command");
        undo.record(std::move(cmd));
        check(p.sequence.video_tracks[0].clips[0].tl_out == 300 &&
                  p.sequence.video_tracks[0].clips[0].src_out == 300,
              "trim: tail clamps to the media duration");

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

        cmd = trim_clip_head(p.sequence, Track::Kind::Video, 0, id, 20, 300);
        check(cmd != nullptr, "trim: head regrow returns command");
        undo.record(std::move(cmd));
        check(p.sequence.video_tracks[0].clips[0].tl_in == 20 &&
                  p.sequence.video_tracks[0].clips[0].src_in == 20,
              "trim: head regrow extends tl_in and src_in together");

        cmd = trim_clip_head(p.sequence, Track::Kind::Video, 0, id, -500, 300);
        check(cmd != nullptr, "trim: over-long head request returns command");
        undo.record(std::move(cmd));
        check(p.sequence.video_tracks[0].clips[0].tl_in == 0 &&
                  p.sequence.video_tracks[0].clips[0].src_in == 0,
              "trim: head clamps to the source start");

        cmd = trim_clip_tail(p.sequence, Track::Kind::Video, 0, id, 210, 300);
        check(cmd == nullptr, "trim: same-position tail is a no-op");

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

        Track& v1 = p.sequence.video_tracks[0];
        v1.locked = true;
        cmd = trim_clip_tail(p.sequence, Track::Kind::Video, 0, id, 60, 300);
        check(cmd == nullptr, "trim: locked track returns nullptr");
        v1.locked = false;
    }

    {
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

        Clip constrict;
        constrict.media = 0;
        constrict.name = "CONSTRICT";
        constrict.tl_in = 70;
        constrict.src_in = 0;
        constrict.src_out = 10;
        cmd = place_clip(p.sequence, Track::Kind::Audio, 0, constrict, Placement::Overwrite);
        check(cmd != nullptr, "trim-linked: place constricting audio clip");
        undo.record(std::move(cmd));

        cmd = trim_clip_tail(p.sequence, Track::Kind::Video, 0, vid, 200, 300);
        check(cmd != nullptr, "trim-linked: mate-constrained tail returns command");
        undo.record(std::move(cmd));
        check(p.sequence.video_tracks[0].clips[0].tl_out == 70 &&
                  p.sequence.audio_tracks[0].clips[0].tl_out == 70,
              "trim-linked: pair tail clamps to the mate's neighbor");
        check(p.sequence.video_tracks[0].clips[0].src_out == 70 &&
                  p.sequence.audio_tracks[0].clips[0].src_out == 70,
              "trim-linked: pair src_out follows in lockstep");

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

    {
        Project p;
        p.sequence.fps = 30.0;
        p.sequence.video_tracks.emplace_back(Track::Kind::Video, "V1");
        p.sequence.audio_tracks.emplace_back(Track::Kind::Audio, "A1");
        p.sequence.video_tracks.emplace_back(Track::Kind::Video, "V2");

        Clip vp;
        vp.media = 3;
        vp.name = "VES_PAIR";
        vp.tl_in = 1000;
        vp.tl_out = 5000;
        vp.src_in = 0;
        vp.src_out = 4000;
        p.sequence.next_clip_id = 900;
        vp.id = p.sequence.next_clip_id++;
        p.sequence.video_tracks[0].clips.push_back(vp);

        Clip ap;
        ap.media = 3;
        ap.name = "AES_PAIR";
        ap.tl_in = 1000;
        ap.tl_out = 5000;
        ap.src_in = 0;
        ap.src_out = 4000;
        ap.id = p.sequence.next_clip_id++;
        p.sequence.audio_tracks[0].clips.push_back(ap);

        Clip vo;
        vo.media = 9;
        vo.name = "VES_VO";
        vo.tl_in = 1000;
        vo.tl_out = 5000;
        vo.id = p.sequence.next_clip_id++;
        p.sequence.audio_tracks[0].clips.push_back(vo);

        Clip dead;
        dead.media = 2;
        dead.name = "VES_V2";
        dead.tl_in = 1000;
        dead.tl_out = 5000;
        dead.id = p.sequence.next_clip_id++;
        p.sequence.video_tracks[1].clips.push_back(dead);

        auto cmd = blade_linked_at(p.sequence, Track::Kind::Video, 0, 2000);
        check(cmd != nullptr, "sibling-blade: video blade returns a command");
        check(p.sequence.video_tracks[0].clips.size() == 2, "sibling-blade: video splits in two");
        check(p.sequence.audio_tracks[0].clips.size() == 3, "sibling-blade: audio clip count unchanged");
        check(p.sequence.video_tracks[1].clips.size() == 1,
              "sibling-blade: same-media clip on a different video track is NOT cut");

        const Clip* vL = p.sequence.video_tracks[0].clip_with_id(vp.id);
        const Clip* vR = [&] {
            for (const auto& c : p.sequence.video_tracks[0].clips)
                if (c.id != vp.id) return &c;
            return (const Clip*)nullptr;
        }();
        const Clip* aL = p.sequence.audio_tracks[0].clip_with_id(ap.id);
        const Clip* aR = [&] {
            for (const auto& c : p.sequence.audio_tracks[0].clips)
                if (c.id != ap.id && c.name == "AES_PAIR") return &c;
            return (const Clip*)nullptr;
        }();
        check(vL && vR && vL->tl_out == 2000 && vR->tl_in == 2000,
              "sibling-blade: video seam at cut frame");
        check(aL && aR && aL->tl_out == 2000 && aR->tl_in == 2000,
              "sibling-blade: audio sibling cut at the same frame");
        check(vL->linked_id == aL->id && aL->linked_id == vL->id,
              "sibling-blade: left halves re-linked");
        check(vR->linked_id == aR->id && aR->linked_id == vR->id,
              "sibling-blade: right halves re-linked");

        cmd->undo(p.sequence);
        check(p.sequence.video_tracks[0].clips.size() == 1 &&
                  p.sequence.audio_tracks[0].clips.size() == 2,
              "sibling-blade: undo restores the pre-cut state");

        Track& at = p.sequence.audio_tracks[0];
        at.locked = true;
        cmd = blade_linked_at(p.sequence, Track::Kind::Video, 0, 3000);
        check(cmd != nullptr, "sibling-blade: primary cut still lands on a locked A1");
        check(at.clip_with_id(ap.id) != nullptr && at.clips.size() == 2,
              "sibling-blade: locked audio sibling is not cut");
    }

    {
        Project p = make_project();
        UndoStack undo;
        Clip a;
        a.media = 0;
        a.tl_in = 0;
        a.src_in = 0;
        a.src_out = 30;
        auto cmd = place_clip(p.sequence, Track::Kind::Video, 0, a, Placement::Overwrite);
        check(cmd != nullptr, "grade: place video clip");
        undo.record(std::move(cmd));
        const ClipId id = p.sequence.video_tracks[0].clips[0].id;

        grade_graph::GradeGraph g;
        const int corr = g.add_node(grade_graph::NodeKind::kCorrector);
        g.node(corr).correct_mode = grade_graph::CorrectMode::kLgg;
        g.node(corr).lgg.emplace();
        g.node(corr).lgg->gain_master = 0.6f;
        const int out_n = g.add_node(grade_graph::NodeKind::kOutput);
        check(g.add_rgb_edge(corr, out_n) >= 0, "grade: wire corrector -> output");

        cmd = set_clip_grade(p.sequence, Track::Kind::Video, 0, id, g);
        check(cmd != nullptr, "grade: set_clip_grade returns command");
        undo.record(std::move(cmd));
        const Clip& gc0 = p.sequence.video_tracks[0].clips[0];
        check(gc0.has_grade(), "grade: clip reports a grade");
        check(gc0.grade.num_nodes() == 2 && gc0.grade.edges().size() == 1,
              "grade: clip carries the full node tree");

        check(undo.undo(p.sequence), "grade: undo");
        check(!p.sequence.video_tracks[0].clips[0].has_grade(),
              "grade: undo restores the no-grade default");
        check(undo.redo(p.sequence), "grade: redo");
        check(p.sequence.video_tracks[0].clips[0].has_grade() &&
                  p.sequence.video_tracks[0].clips[0].grade.num_nodes() == 2,
              "grade: redo restores the tree");

        std::string err;
        check(save_project(p, "/tmp/opencode/media/grade.ehproj", &err),
              "grade: save project (v4)");
        Project loaded;
        check(load_project(loaded, "/tmp/opencode/media/grade.ehproj", &err),
              "grade: load project");
        const Clip& lc = loaded.sequence.video_tracks[0].clips[0];
        check(lc.has_grade() && lc.grade.num_nodes() == 2 && lc.grade.edges().size() == 1,
              "grade: tree survives save/load");
        bool gain_ok = false;
        for (int i = 0; i < static_cast<int>(lc.grade.num_nodes()); ++i) {
            const auto& n = lc.grade.node(i);
            if (n.lgg && n.lgg->gain_master == 0.6f) gain_ok = true;
        }
        check(gain_ok, "grade: node params survive save/load");

        nlohmann::json v3;
        v3["canvas_project"] = 3;
        v3["name"] = "legacy";
        v3["fps"] = 30.0;
        v3["next_clip_id"] = 2;
        nlohmann::json media = nlohmann::json::array();
        nlohmann::json m0;
        m0["id"] = 0;
        m0["path"] = "/tmp/x.mp4";
        m0["fps"] = 30.0;
        m0["width"] = 100;
        m0["height"] = 50;
        m0["total_frames"] = 10;
        m0["bin"] = "B";
        m0["has_audio"] = false;
        media.push_back(m0);
        v3["media"] = media;
        v3["bins"] = nlohmann::json::array({"B"});
        nlohmann::json tracks = nlohmann::json::array();
        nlohmann::json clip;
        clip["id"] = 1;
        clip["media"] = 0;
        clip["tl_in"] = 0;
        clip["tl_out"] = 10;
        clip["src_in"] = 0;
        clip["src_out"] = 10;
        clip["name"] = "L";
        nlohmann::json clips = nlohmann::json::array();
        clips.push_back(clip);
        nlohmann::json v1;
        v1["name"] = "V1";
        v1["locked"] = false;
        v1["muted"] = false;
        v1["solo"] = false;
        v1["gain_db"] = 0.0;
        v1["clips"] = clips;
        tracks.push_back(v1);
        v3["video_tracks"] = tracks;
        v3["audio_tracks"] = nlohmann::json::array();
        const std::string v3path = "/tmp/opencode/media/legacy_v3.ehproj";
        {
            std::ofstream out(v3path);
            out << v3.dump(2);
        }
        Project legacy;
        check(load_project(legacy, v3path, &err), "grade: legacy v3 project loads");
        check(legacy.sequence.video_tracks.size() == 1 &&
                  legacy.sequence.video_tracks[0].clips.size() == 1,
              "grade: legacy clip present");
        check(!legacy.sequence.video_tracks[0].clips[0].has_grade(),
              "grade: legacy clip has no grade");
        check(save_project(legacy, "/tmp/opencode/media/legacy_v3_resaved.ehproj", &err),
              "grade: legacy project re-saves");
        {
            std::ifstream in("/tmp/opencode/media/legacy_v3_resaved.ehproj");
            const std::string raw((std::istreambuf_iterator<char>(in)),
                                  std::istreambuf_iterator<char>());
            check(raw.find("\"grade\"") == std::string::npos,
                  "grade: ungraded clip re-saves without a grade key");
        }

        Project lp = make_project();
        Clip v;
        v.media = 0;
        v.tl_in = 0;
        v.src_in = 0;
        v.src_out = 30;
        cmd = place_clip(lp.sequence, Track::Kind::Video, 0, v, Placement::Overwrite);
        undo.record(std::move(cmd));
        Clip au = v;
        au.media = 0;
        cmd = place_clip(lp.sequence, Track::Kind::Audio, 0, au, Placement::Overwrite);
        undo.record(std::move(cmd));
        const ClipId vid = lp.sequence.video_tracks[0].clips[0].id;
        cmd = link_clip(lp.sequence, Track::Kind::Video, 0, vid);
        undo.record(std::move(cmd));
        cmd = set_clip_grade(lp.sequence, Track::Kind::Video, 0, vid, g);
        check(cmd != nullptr, "grade: set on linked video returns command");
        check(lp.sequence.audio_tracks[0].clips[0].grade.num_nodes() == 2,
              "grade: linked audio mate inherits the grade");
    }

    {
        std::unique_ptr<ICommand> cmd;
        Project tp = make_project();
        Clip tv;
        tv.media = 0;
        tv.tl_in = 0;
        tv.src_in = 0;
        tv.src_out = 30;
        cmd = place_clip(tp.sequence, Track::Kind::Video, 0, tv, Placement::Overwrite);
        undo.record(std::move(cmd));
        const ClipId tid = tp.sequence.video_tracks[0].clips[0].id;

        Clip::Title want;
        want.text = "Hello";
        want.size = 0.15f;
        want.r = 0.8f;
        want.g = 0.1f;
        want.b = 0.2f;
        want.a = 0.9f;
        want.font_family = "DejaVu Sans";
        want.bold = true;
        want.italic = true;
        want.underline = true;
        want.shadow = true;
        want.shadow_dx = 3.5f;
        want.shadow_dy = -2.0f;
        want.shadow_blur = 4.0f;
        want.shadow_opacity = 0.4f;
        want.shadow_r = 0.5f;
        want.shadow_g = 0.0f;
        want.shadow_b = 0.5f;
        want.box = true;
        want.box_pad_x = 14.0f;
        want.box_pad_y = 6.0f;
        want.box_radius = 9.0f;
        want.box_opacity = 0.25f;
        want.box_r = 0.0f;
        want.box_g = 0.5f;
        want.box_b = 0.5f;
        cmd = set_clip_title(tp.sequence, Track::Kind::Video, 0, tid, want);
        check(cmd != nullptr, "title: set returns command");
        undo.record(std::move(cmd));
        check(tp.sequence.video_tracks[0].clips[0].has_title(),
              "title: has_title after set");

        Clip::Title huge = want;
        huge.size = 9.9f;
        cmd = set_clip_title(tp.sequence, Track::Kind::Video, 0, tid, huge);
        check(cmd != nullptr, "title: clamped set still returns command");
        undo.record(std::move(cmd));
        check(tp.sequence.video_tracks[0].clips[0].title.size == title::kSizeMax,
              "title: size clamps to max");
        check(tp.sequence.video_tracks[0].clips[0].title.text == want.text,
              "title: text untouched by clamp");

        check(undo.undo(tp.sequence), "title: undo step");
        check(tp.sequence.video_tracks[0].clips[0].title.text == want.text &&
                  tp.sequence.video_tracks[0].clips[0].title.size == want.size,
              "title: undo restores the pre-clamp title");
        check(undo.undo(tp.sequence) && !tp.sequence.video_tracks[0].clips[0].has_title(),
              "title: undo clears the title");
        check(undo.redo(tp.sequence) && tp.sequence.video_tracks[0].clips[0].has_title(),
              "title: redo restores the title");

        std::string terr;
        check(save_project(tp, "/tmp/opencode/media/title.ehproj", &terr),
              "title: save project (v4)");
        Project tloaded;
        check(load_project(tloaded, "/tmp/opencode/media/title.ehproj", &terr),
              "title: load project");
        const Clip& tlc = tloaded.sequence.video_tracks[0].clips[0];
        check(tlc.has_title() && tlc.title.text == want.text && tlc.title.size == want.size &&
                  tlc.title.r == want.r && tlc.title.g == want.g && tlc.title.b == want.b &&
                  tlc.title.a == want.a && tlc.title.font_family == want.font_family &&
                  tlc.title.bold && tlc.title.italic && tlc.title.underline &&
                  tlc.title.shadow && tlc.title.shadow_dx == want.shadow_dx &&
                  tlc.title.shadow_dy == want.shadow_dy &&
                  tlc.title.shadow_blur == want.shadow_blur &&
                  tlc.title.shadow_opacity == want.shadow_opacity &&
                  tlc.title.shadow_r == want.shadow_r && tlc.title.shadow_g == want.shadow_g &&
                  tlc.title.shadow_b == want.shadow_b && tlc.title.box &&
                  tlc.title.box_pad_x == want.box_pad_x &&
                  tlc.title.box_pad_y == want.box_pad_y &&
                  tlc.title.box_radius == want.box_radius &&
                  tlc.title.box_opacity == want.box_opacity && tlc.title.box_r == want.box_r &&
                  tlc.title.box_g == want.box_g && tlc.title.box_b == want.box_b,
              "title: fields survive save/load");

        std::string nt_err;
        check(save_project(tloaded, "/tmp/opencode/media/notitle.ehproj", &nt_err),
              "title: save project with title cleared");
    }

    {
        Project gp;
        canvas::core::Track gv1;
        gv1.kind = canvas::core::Track::Kind::Video;
        gv1.name = "V1";
        Clip g1;
        g1.id = 1;
        g1.media = 0;
        g1.tl_in = 0;
        g1.src_in = 0;
        g1.src_out = 60;
        Clip g2;
        g2.id = 2;
        g2.media = 0;
        g2.tl_in = 60;
        g2.src_in = 0;
        g2.src_out = 60;
        gv1.clips.push_back(g1);
        gv1.clips.push_back(g2);
        gp.sequence.video_tracks.push_back(std::move(gv1));
        UndoStack gundo;

        std::vector<std::unique_ptr<ICommand>> children;
        Clip::Title gtitle;
        gtitle.text = "hello";
        gtitle.size = 0.12f;
        children.push_back(set_clip_title(gp.sequence, Track::Kind::Video, 0, 1, gtitle));
        children.push_back(set_clip_transform(gp.sequence, Track::Kind::Video, 0, 2, 1.0, 1.0,
                                              120.0, -60.0, 0.0, 0.0, 0.0, false, false));
        auto group = std::make_unique<GroupCommand>("Bulk Captions", std::move(children));
        check(group != nullptr && !group->name().empty(), "group: constructed with its name");
        gundo.record(std::move(group));
        check(gp.sequence.video_tracks[0].clips[0].title.text == "hello",
              "group: redo applies child 1 (title)");
        check(gp.sequence.video_tracks[0].clips[1].pos_x == 120.0 &&
                  gp.sequence.video_tracks[0].clips[1].pos_y == -60.0,
              "group: redo applies child 2 (transform)");

        check(gundo.undo(gp.sequence), "group: one undo reverts the whole batch");
        check(!gp.sequence.video_tracks[0].clips[0].has_title(),
              "group: undo restores child 1 (title cleared)");
        check(gp.sequence.video_tracks[0].clips[1].pos_x == 0.0 &&
                  gp.sequence.video_tracks[0].clips[1].pos_y == 0.0,
              "group: undo restores child 2 (transform reverted)");

        check(gundo.redo(gp.sequence), "group: redo restores the whole batch");
        check(gp.sequence.video_tracks[0].clips[0].title.text == "hello" &&
                  gp.sequence.video_tracks[0].clips[1].pos_x == 120.0,
              "group: redo restores both children");
    }

    {
        Project tp;
        tp.name = "utf8-repair";
        canvas::core::Track v1;
        v1.kind = canvas::core::Track::Kind::Video;
        v1.name = "V1";
        Clip c;
        c.id = 1;
        c.media = 0;
        c.tl_in = 0;
        c.tl_out = 24;
        c.src_in = 0;
        c.src_out = 24;
        std::string bad = "label\xFF\xFE";
        c.comments = bad;
        c.eq_bands[0].frequency = 1000.0f;
        c.eq_bands[0].gain = std::nan("");
        c.eq_bands[0].enabled = true;
        v1.clips.push_back(std::move(c));
        tp.sequence.video_tracks.push_back(std::move(v1));
        std::string err;
        check(save_project(tp, "/tmp/opencode/media/utf8_repair.ncs", &err),
              "utf8: save with invalid bytes succeeds");
        Project loaded;
        check(load_project(loaded, "/tmp/opencode/media/utf8_repair.ncs", &err),
              "utf8: repaired project loads");
        const Clip& lc = loaded.sequence.video_tracks[0].clips[0];
        check(lc.comments == "label\xEF\xBF\xBD\xEF\xBF\xBD",
              "utf8: comments repaired to U+FFFD");
        check(std::isfinite(lc.eq_bands[0].gain),
              "utf8: NaN gain repaired to a finite value (dump no longer throws)");
        check(std::ifstream("/tmp/opencode/media/utf8_repair.ncs").good() &&
                  std::ifstream("/tmp/opencode/media/utf8_repair.ncs").peek() != EOF,
              "utf8: output file is non-empty (no 0-byte save)");
        std::remove("/tmp/opencode/media/utf8_repair.ncs");
    }

    if (failures == 0) {
        std::printf("ALL TESTS PASSED\n");
        return 0;
    }
    std::fprintf(stderr, "%d TEST(S) FAILED\n", failures);
    return 1;
}
