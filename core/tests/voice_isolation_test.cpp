// Voice-isolation test: the headless engine seam (VoiceIsolation /
// VoiceIsolationBank), the per-clip model field, its edit op (incl. linked-mate
// inheritance + undo), and project JSON round-trip. The RNNoise stage runs a
// real network on synthetic PCM — the assertions pin the STREAMING contract,
// not the denoise quality (that is a perceptual property, not a unit one).

#include "canvas/core/media/voice_isolation.hpp"
#include "canvas/core/project/project.hpp"
#include "canvas/core/timeline/edit_ops.hpp"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace canvas::core;

namespace {

int failures = 0;

void check(bool cond, const char* what) {
    if (!cond) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++failures;
    } else {
        std::printf("ok:   %s\n", what);
    }
}

bool all_finite(const float* v, int n) {
    for (int i = 0; i < n; ++i) {
        if (!std::isfinite(v[i])) return false;
    }
    return true;
}

Project make_project() {
    Project p;
    p.name = "VoiceIsolation";
    p.sequence.fps = 30.0;

    MediaEntry m0;
    m0.id = 0;
    m0.path = "/tmp/opencode/media/vi_test.mp4";
    m0.fps = 30.0;
    m0.width = 1280;
    m0.height = 720;
    m0.total_frames = 300;
    p.media.push_back(m0);

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

void test_streaming() {
    VoiceIsolation vi;

    // Mono 48k block-aligned sine: every call returns a full 480-frame block.
    std::vector<float> buf(480);
    for (int call = 0; call < 4; ++call) {
        for (int i = 0; i < 480; ++i)
            buf[static_cast<std::size_t>(i)] =
                static_cast<float>(0.25 * std::sin(2.0 * 3.141592653589793 * 440.0 * i / 48000.0));
        const int got = vi.process(48000, 1, buf.data(), 480);
        check(got == 480, "block-aligned call outputs a full frame");
        check(all_finite(buf.data(), 480), "RNNoise output is finite");
    }

    // Sub-block pipe: a 256-frame call that cannot complete a 480 block emits
    // nothing (lookahead), then drains the backlog in later calls. Never write
    // more than requested, never read uninitialized memory.
    VoiceIsolation vs;
    std::vector<float> small(256);
    for (int i = 0; i < 256; ++i)
        small[static_cast<std::size_t>(i)] = 0.1f;
    int got = vs.process(48000, 1, small.data(), 256);
    check(got == 0, "first sub-block call emits nothing (priming lookahead)");
    got = vs.process(48000, 1, small.data(), 256);
    check(got >= 0 && got <= 256, "second sub-block call drains backlog in-bounds");
    check(all_finite(small.data(), 256), "drained sub-block output is finite");

    // Wrong rate / channel count: engine refuses (writes nothing, returns 0).
    VoiceIsolation bad;
    check(bad.process(44100, 1, buf.data(), 480) == 0, "44.1 kHz refused by engine");
    check(bad.process(48000, 0, buf.data(), 480) == 0, "0 channels refused by engine");

    // Stereo end-to-end runs without crashing and stays in-bounds.
    VoiceIsolation st;
    std::vector<float> stbuf(480 * 2);
    got = st.process(48000, 2, stbuf.data(), 480);
    check(got == 480, "stereo block-aligned call outputs a full frame");
    check(all_finite(stbuf.data(), 480 * 2), "stereo output is finite");
}

void test_bank() {
    VoiceIsolationBank bank;
    std::vector<float> buf(480, 0.125f);

    // Pass-through when nothing is wanted: None and unsupported modes return
    // num_frames untouched, preserving the caller's audio.
    check(bank.tick(1, VoiceIsolationMode::None, 48000, 1, buf.data(), 480) == 480,
          "None mode is a pass-through");
    check(bank.tick(1, VoiceIsolationMode::DeepFilterNet, 48000, 1, buf.data(), 480) == 480,
          "unsupported mode is a pass-through");

    // Active engine runs; the bank streams per clip id.
    const int got = bank.tick(7, VoiceIsolationMode::RnNoise, 48000, 1, buf.data(), 480);
    check(got > 0 && got <= 480, "RNNoise tick streams frames for clip 7");
    check(all_finite(buf.data(), 480), "RNNoise tick output is finite");

    // A different clip id keeps independent state (no crash, sane count).
    std::vector<float> buf2(480, 0.1f);
    const int got2 = bank.tick(8, VoiceIsolationMode::RnNoise, 48000, 1, buf2.data(), 480);
    check(got2 > 0 && got2 <= 480, "RNNoise tick streams frames for clip 8");

    // Mode flip resets the network for that clip (fresh state, again a full
    // block from the first post-flip call).
    std::vector<float> buf3(480, 0.1f);
    int g1 = bank.tick(7, VoiceIsolationMode::RnNoise, 48000, 1, buf3.data(), 480);
    const int gnone = bank.tick(7, VoiceIsolationMode::None, 48000, 1, buf3.data(), 480);
    int g2 = bank.tick(7, VoiceIsolationMode::RnNoise, 48000, 1, buf3.data(), 480);
    check(gnone == 480, "None tick after RnNoise is a pass-through");
    check(g1 > 0 && g1 <= 480 && g2 > 0 && g2 <= 480, "mode flip runs a fresh network");

    // drop()/clear() leave the bank usable (state is dropped, not corrupted).
    bank.drop();
    bank.drop(7);
    bank.clear();
    const int g3 = bank.tick(7, VoiceIsolationMode::RnNoise, 48000, 1, buf3.data(), 480);
    check(g3 > 0 && g3 <= 480, "bank usable after drop/clear");
}

void test_edit_op_and_roundtrip() {
    UndoStack undo;
    {
        Project p = make_project();
        Clip v;
        v.media = 0;
        v.name = "V";
        v.tl_in = 0;
        v.src_in = 0;
        v.src_out = 60;
        Clip a = v;
        a.name = "A";
        auto cmd = place_linked_clip(p.sequence, 0, 0, v, a, Placement::Overwrite);
        check(cmd != nullptr, "vi: place linked pair");
        undo.record(std::move(cmd));

        const auto& vc = p.sequence.video_tracks[0].clips[0];
        const auto& ac = p.sequence.audio_tracks[0].clips[0];
        check(vc.voice_isolation == VoiceIsolationMode::None &&
              ac.voice_isolation == VoiceIsolationMode::None,
              "vi: default is None");
        check(vc.is_linked() && ac.is_linked(), "vi: pair is linked");

        // Set on the audio half: both halves inherit the mode.
        cmd = set_clip_voice_isolation(p.sequence, Track::Kind::Audio, 0, ac.id,
                                       VoiceIsolationMode::RnNoise);
        check(cmd != nullptr, "vi: set_clip_voice_isolation returns command");
        undo.record(std::move(cmd));
        check(p.sequence.audio_tracks[0].clips[0].voice_isolation == VoiceIsolationMode::RnNoise &&
              p.sequence.video_tracks[0].clips[0].voice_isolation == VoiceIsolationMode::RnNoise,
              "vi: linked mate inherits the mode");

        // Unknown clip -> nullptr (no crash).
        cmd = set_clip_voice_isolation(p.sequence, Track::Kind::Video, 0, 9999,
                                       VoiceIsolationMode::RnNoise);
        check(cmd == nullptr, "vi: unknown clip returns nullptr");

        // Undo restores None on both; redo re-applies.
        check(undo.undo(p.sequence), "vi: undo isolation");
        check(p.sequence.audio_tracks[0].clips[0].voice_isolation == VoiceIsolationMode::None &&
              p.sequence.video_tracks[0].clips[0].voice_isolation == VoiceIsolationMode::None,
              "vi: undo restored None");
        check(undo.redo(p.sequence), "vi: redo isolation");
        check(p.sequence.audio_tracks[0].clips[0].voice_isolation == VoiceIsolationMode::RnNoise,
              "vi: redo re-applied the mode");

        // JSON round-trip preserves the mode.
        check(save_project(p, "/tmp/opencode/media/voice_isolation.ehproj") == true,
              "vi: save project");
        Project loaded;
        check(load_project(loaded, "/tmp/opencode/media/voice_isolation.ehproj") == true,
              "vi: load project");
        const auto& lc = loaded.sequence.audio_tracks[0].clips[0];
        check(lc.voice_isolation == VoiceIsolationMode::RnNoise,
              "vi: project round-trip preserves the mode");
    }

    // Mode-name / supported table.
    check(std::string(voice_isolation_mode_name(VoiceIsolationMode::None)) == "None",
          "vi: None name");
    check(voice_isolation_supported(VoiceIsolationMode::RnNoise),
          "vi: RNNoise supported");
    check(!voice_isolation_supported(VoiceIsolationMode::DeepFilterNet),
          "vi: DeepFilterNet not built (reserved seam)");
}

}  // namespace

int main() {
    test_streaming();
    test_bank();
    test_edit_op_and_roundtrip();

    if (failures == 0) {
        std::printf("ALL TESTS PASSED\n");
        return 0;
    }
    std::fprintf(stderr, "%d TEST(S) FAILED\n", failures);
    return 1;
}