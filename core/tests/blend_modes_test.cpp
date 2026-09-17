// Phase 1 (F6) blend-mode LAW tests. Pins the canonical timeline byte blend law
// (timeline/blend.hpp) for all 8 modes:
//  - the enum values / count and the display-name table;
//  - exact known byte values for the simple modes (Normal, Add, Multiply,
//    Screen, Overlay, Subtract, Difference and SoftLight's source==0.5 pivot);
//  - byte parity vs the color-page float law (grade_graph::blend_channel)
//    across a value grid, for every mode (the two renderers must not drift);
//  - the opacity dissolve;
//  - edit-op acceptance + project round-trip for the three newly-admitted modes
//    (SoftLight/Subtract/Difference), which the old 5-mode clamp rejected.
// Headless — links only canvas_core.

#include "canvas/core/grade_graph/composite.hpp"
#include "canvas/core/project/project.hpp"
#include "canvas/core/timeline/blend.hpp"
#include "canvas/core/timeline/edit_ops.hpp"
#include "canvas/core/timeline/visual.hpp"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>

using namespace canvas::core;
namespace gg = canvas::core::grade_graph;

namespace {

int failures = 0;

void check(const bool cond, const char* what) {
    if (!cond) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++failures;
    } else {
        std::printf("ok:   %s\n", what);
    }
}

// Independent oracle: map the timeline enum to the color-page enum by NAME and
// apply the documented float law + dissolve. Deliberately a separate mapping so
// a bug in blend.cpp's mapping cannot hide behind the same bug in the test.
gg::BlendMode to_grade(const BlendMode m) {
    switch (m) {
        case BlendMode::Normal: return gg::BlendMode::kNormal;
        case BlendMode::Add: return gg::BlendMode::kAdd;
        case BlendMode::Multiply: return gg::BlendMode::kMultiply;
        case BlendMode::Screen: return gg::BlendMode::kScreen;
        case BlendMode::Overlay: return gg::BlendMode::kOverlay;
        case BlendMode::SoftLight: return gg::BlendMode::kSoftLight;
        case BlendMode::Subtract: return gg::BlendMode::kSubtract;
        case BlendMode::Difference: return gg::BlendMode::kDifference;
    }
    return gg::BlendMode::kNormal;
}

uint8_t oracle(const BlendMode m, const float op, const uint8_t b, const uint8_t s) {
    const float bf = static_cast<float>(b) / 255.0f;
    const float sf = static_cast<float>(s) / 255.0f;
    const float blended = gg::blend_channel(to_grade(m), bf, sf);
    const float out = blended * op + bf * (1.0f - op);
    const int v = static_cast<int>(std::lround(out * 255.0f));
    return static_cast<uint8_t>(v < 0 ? 0 : (v > 255 ? 255 : v));
}

void test_enum_and_count() {
    check(blend::kBlendModeCount == 8, "count == 8");
    check(visual::kBlendModeCount == blend::kBlendModeCount,
          "visual::kBlendModeCount matches blend::kBlendModeCount");
    check(static_cast<int>(BlendMode::Normal) == 0, "Normal == 0");
    check(static_cast<int>(BlendMode::Add) == 1, "Add == 1");
    check(static_cast<int>(BlendMode::Multiply) == 2, "Multiply == 2");
    check(static_cast<int>(BlendMode::Screen) == 3, "Screen == 3");
    check(static_cast<int>(BlendMode::Overlay) == 4, "Overlay == 4 (legacy values kept)");
    check(static_cast<int>(BlendMode::SoftLight) == 5, "SoftLight == 5 (appended)");
    check(static_cast<int>(BlendMode::Subtract) == 6, "Subtract == 6 (appended)");
    check(static_cast<int>(BlendMode::Difference) == 7, "Difference == 7 (appended)");
    check(blend::valid_blend_mode(BlendMode::Difference), "Difference valid");
    check(!blend::valid_blend_mode(static_cast<BlendMode>(8)), "mode 8 invalid");
}

void test_names() {
    check(std::string(blend::blend_mode_name(BlendMode::Normal)) == "Normal", "name Normal");
    check(std::string(blend::blend_mode_name(BlendMode::Add)) == "Add", "name Add");
    check(std::string(blend::blend_mode_name(BlendMode::Multiply)) == "Multiply", "name Multiply");
    check(std::string(blend::blend_mode_name(BlendMode::Screen)) == "Screen", "name Screen");
    check(std::string(blend::blend_mode_name(BlendMode::Overlay)) == "Overlay", "name Overlay");
    check(std::string(blend::blend_mode_name(BlendMode::SoftLight)) == "Soft Light",
          "name Soft Light");
    check(std::string(blend::blend_mode_name(BlendMode::Subtract)) == "Subtract", "name Subtract");
    check(std::string(blend::blend_mode_name(BlendMode::Difference)) == "Difference",
          "name Difference");
}

void test_known_values() {
    constexpr float kOp = 1.0f;
    check(blend::blend_channel(BlendMode::Normal, kOp, 10, 200) == 200, "Normal == src");
    check(blend::blend_channel(BlendMode::Add, kOp, 100, 100) == 200, "Add sums");
    check(blend::blend_channel(BlendMode::Add, kOp, 200, 200) == 255, "Add clamps at 255");
    check(blend::blend_channel(BlendMode::Multiply, kOp, 255, 128) == 128, "Multiply 255*128");
    check(blend::blend_channel(BlendMode::Multiply, kOp, 0, 200) == 0, "Multiply by 0");
    check(blend::blend_channel(BlendMode::Screen, kOp, 255, 33) == 255, "Screen with 255");
    check(blend::blend_channel(BlendMode::Screen, kOp, 0, 77) == 77, "Screen with 0 == src");
    check(blend::blend_channel(BlendMode::Overlay, kOp, 255, 33) == 255, "Overlay base 255");
    check(blend::blend_channel(BlendMode::Overlay, kOp, 0, 33) == 0, "Overlay base 0");
    check(blend::blend_channel(BlendMode::Subtract, kOp, 255, 255) == 0, "Subtract equal -> 0");
    check(blend::blend_channel(BlendMode::Subtract, kOp, 10, 200) == 0, "Subtract clamps at 0");
    check(blend::blend_channel(BlendMode::Difference, kOp, 255, 0) == 255, "Difference 255/0");
    check(blend::blend_channel(BlendMode::Difference, kOp, 128, 128) == 0, "Difference equal -> 0");
    // SoftLight around the source == 0.5 pivot: the correction term vanishes, so
    // the result is the backdrop (within a byte).
    const int sl = blend::blend_channel(BlendMode::SoftLight, kOp, 100, 127);
    check(std::abs(sl - 100) <= 1, "SoftLight source~0.5 preserves backdrop");

    // Opacity dissolve: Normal at 50% over black is half the source.
    check(blend::blend_channel(BlendMode::Normal, 0.5f, 0, 100) == 50, "Normal @50% dissolve");
    check(blend::blend_channel(BlendMode::Add, 0.5f, 0, 100) == 50, "Add @50% dissolve");
    check(blend::blend_channel(BlendMode::Normal, 0.0f, 200, 0) == 200, "opacity 0 -> backdrop");
    check(blend::blend_channel(BlendMode::Normal, 1.0f, 200, 7) == 7, "opacity 1 -> src");
}

void test_parity_grid() {
    const std::uint8_t vals[] = {0, 1, 17, 64, 127, 128, 200, 254, 255};
    const float ops[] = {0.0f, 0.25f, 0.5f, 0.75f, 1.0f};
    int max_diff = 0;
    for (int m = 0; m < blend::kBlendModeCount; ++m) {
        const auto mode = static_cast<BlendMode>(m);
        for (const std::uint8_t b : vals)
            for (const std::uint8_t s : vals)
                for (const float op : ops) {
                    const int got = static_cast<int>(blend::blend_channel(mode, op, b, s));
                    const int want = static_cast<int>(oracle(mode, op, b, s));
                    const int diff = got > want ? got - want : want - got;
                    if (diff > max_diff) max_diff = diff;
                    if (diff > 1) {
                        std::fprintf(stderr, "mismatch mode=%d op=%.2f b=%u s=%u got=%d want=%d\n",
                                     m, static_cast<double>(op), b, s, got, want);
                    }
                }
    }
    std::printf("      (max byte difference vs float law: %d)\n", max_diff);
    check(max_diff <= 1, "uint8 law within 1 byte of grade_graph float law (all 8 modes)");
}

void test_edit_and_json_roundtrip() {
    Project p;
    p.name = "F6";
    p.sequence.fps = 30.0;
    MediaEntry m;
    m.id = 7;
    m.path = "/tmp/canvas_f6.mov";
    m.fps = 30.0;
    m.width = 64;
    m.height = 48;
    m.total_frames = 300;
    p.media.push_back(m);

    Track v;
    v.kind = Track::Kind::Video;
    v.name = "V1";
    Clip c;
    c.id = 1;
    c.media = 7;
    c.tl_in = 0;
    c.tl_out = 50;
    c.src_in = 0;
    c.src_out = 50;
    v.clips.push_back(c);
    p.sequence.video_tracks.push_back(v);
    p.sequence.next_clip_id = 2;

    const BlendMode new_modes[] = {BlendMode::SoftLight, BlendMode::Subtract, BlendMode::Difference};
    for (const BlendMode mode : new_modes) {
        const BlendMode prior = p.sequence.video_tracks[0].clips[0].blend_mode;
        auto cmd = set_clip_composite(p.sequence, Track::Kind::Video, 0, 1, 1.0f, mode);
        check(cmd != nullptr, "set_clip_composite accepts appended mode");
        check(p.sequence.video_tracks[0].clips[0].blend_mode == mode,
              "clip carries appended blend mode");
        // Undo/redo round-trip.
        cmd->undo(p.sequence);
        check(p.sequence.video_tracks[0].clips[0].blend_mode == prior,
              "undo restores the prior blend mode");
        cmd->redo(p.sequence);
        check(p.sequence.video_tracks[0].clips[0].blend_mode == mode, "redo restores mode");

        const std::string path = "/tmp/canvas_f6_blend.ncs";
        std::string err;
        check(save_project(p, path, &err), "save project with appended blend mode");
        Project q;
        check(load_project(q, path, &err), "load project with appended blend mode");
        check(q.sequence.video_tracks.size() == 1 &&
                  q.sequence.video_tracks[0].clips.size() == 1 &&
                  q.sequence.video_tracks[0].clips[0].blend_mode == mode,
              "appended blend mode survives JSON round-trip");
    }
    // A full 8-mode sweep through the clamp: every mode is accepted.
    for (int i = 0; i < blend::kBlendModeCount; ++i) {
        auto cmd = set_clip_composite(p.sequence, Track::Kind::Video, 0, 1, 1.0f,
                                      static_cast<BlendMode>(i));
        check(cmd != nullptr &&
                  static_cast<int>(p.sequence.video_tracks[0].clips[0].blend_mode) == i,
              "set_clip_composite accepts every mode 0..7");
    }
}

}  // namespace

int main() {
    test_enum_and_count();
    test_names();
    test_known_values();
    test_parity_grid();
    test_edit_and_json_roundtrip();

    if (failures == 0) {
        std::printf("ALL TESTS PASSED\n");
        return 0;
    }
    std::fprintf(stderr, "%d FAILURES\n", failures);
    return 1;
}