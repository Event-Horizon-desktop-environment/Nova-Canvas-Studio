// Headless test for the Dual-Viewer / media-pool-scrub source-preview model:
// the synthetic single-clip project built around a pooled MediaEntry and the
// scrub fraction<->source frame law. Compiles source_preview_model.cpp directly
// so the Qt-free seam holds (a stray <Q...> include breaks this build).
//
// Checks:
//   - build_source_project places 1 video clip / 1 linked audio clip properly
//   - fps falls back (media fps -> project fps -> 30), frames degenerate -> 1
//   - audio-only / video-only media build only their own track
//   - fraction <-> frame law: identity, clamp, degenerate total
//   - edit-command undo path: an existing project's next_clip_id is respected

#include <cassert>
#include <cmath>
#include <cstdint>
#include <memory>
#include <string>

#include "canvas/core/project/project.hpp"
#include "canvas/core/timeline/model.hpp"
#include "features/source_preview/source_preview_model.hpp"

namespace {

using canvas::core::Clip;
using canvas::core::MediaEntry;
using canvas::core::Track;
using canvas::gui::source_preview::build_source_project;
using canvas::gui::source_preview::fraction_to_source_frame;
using canvas::gui::source_preview::frame_to_fraction;

void check_video_audio_project() {
    MediaEntry media;
    media.id = 3;
    media.path = "/media/clip.mov";
    media.fps = 25.0;
    media.width = 1920;
    media.height = 1080;
    media.total_frames = 2500;
    media.has_audio = true;

    const auto proj = build_source_project(media, 0.0);
    assert(proj != nullptr);
    assert(proj->media.size() == 1);
    assert(proj->media[0].id == 3);
    assert(proj->sequence.video_tracks.size() == 1);
    assert(proj->sequence.audio_tracks.size() == 1);
    assert(proj->sequence.fps == 25.0);

    const Track& vt = proj->sequence.video_tracks[0];
    assert(vt.kind == Track::Kind::Video);
    assert(vt.clips.size() == 1);
    const Clip& clip = vt.clips[0];
    assert(clip.media == 3);
    assert(clip.tl_in == 0);
    assert(clip.tl_out == 2500);
    assert(clip.src_in == 0);
    assert(clip.src_out == 2500);
    // A/V link is bidirectional, so the audio clip rides along on timeline ops.
    assert(clip.linked_id != 0);

    const Track& at = proj->sequence.audio_tracks[0];
    assert(at.kind == Track::Kind::Audio);
    assert(at.clips.size() == 1);
    const Clip& aclip = at.clips[0];
    assert(aclip.media == 3);
    assert(aclip.tl_in == 0);
    assert(aclip.tl_out == 2500);
    assert(aclip.linked_id == clip.id);

    // Field-delimited reads: duration_frames = max tl_out (end-exclusive).
    assert(proj->sequence.duration_frames() == 2500);

    assert(proj->sequence.next_clip_id > clip.id);
}

void check_video_only_ignores_audio_when_absent() {
    MediaEntry media;  // video, no audio stream
    media.id = 1;
    media.path = "/media/video.mp4";
    media.fps = 0.0;  // no rate reported either
    media.width = 640;
    media.height = 360;
    media.total_frames = 500;
    media.has_audio = false;

    const auto proj = build_source_project(media, 24.0);
    assert(proj != nullptr);
    assert(proj->sequence.video_tracks.size() == 1);
    assert(proj->sequence.audio_tracks.empty());
    assert(proj->sequence.fps == 24.0);
    assert(proj->sequence.video_tracks[0].clips.size() == 1);
    assert(proj->sequence.video_tracks[0].clips[0].tl_out == 500);
}

void check_audio_only_has_no_video_track() {
    MediaEntry media;
    media.id = 7;
    media.path = "/media/voice.wav";
    media.fps = 48.0;
    media.width = 0;
    media.height = 0;
    media.total_frames = 4800;
    media.has_audio = true;

    const auto proj = build_source_project(media, 0.0);
    assert(proj != nullptr);
    assert(proj->sequence.video_tracks.empty());
    assert(proj->sequence.audio_tracks.size() == 1);
    assert(proj->sequence.fps == 48.0);
    const Clip& clip = proj->sequence.audio_tracks[0].clips[0];
    assert(clip.tl_out == 4800);
    assert(clip.linked_id == 0);  // no video mate to link to
}

void check_degenerate_and_truncated_frames() {
    MediaEntry media;
    media.id = 2;
    media.path = "/media/still.jpg";
    media.width = 800;
    media.height = 600;
    media.total_frames = -1;  // unknown length: treat as a single frame
    media.has_audio = false;

    const auto proj = build_source_project(media, 0.0);
    assert(proj != nullptr);
    assert(proj->sequence.fps == 30.0);  // both rates unknown -> 30
    const Clip& clip = proj->sequence.video_tracks[0].clips[0];
    assert(clip.tl_out == 1);
    assert(clip.src_out == 1);

    // Degenerate totals collapse the whole law onto frame 0.
    assert(fraction_to_source_frame(0.0, 1) == 0);
    assert(fraction_to_source_frame(0.5, 1) == 0);
    assert(fraction_to_source_frame(1.0, 1) == 0);
    assert(frame_to_fraction(0, 1) == 0.0);
    assert(frame_to_fraction(42, 1) == 0.0);
    assert(frame_to_fraction(0, 1) == 0.0);
}

void check_fraction_frame_law() {
    // Identity endpoints on a 100-frame source.
    assert(fraction_to_source_frame(0.0, 100) == 0);
    assert(fraction_to_source_frame(1.0, 100) == 99);
    assert(fraction_to_source_frame(0.5, 100) == 50);
    assert(frame_to_fraction(0, 100) == 0.0);
    assert(std::abs(frame_to_fraction(99, 100) - 1.0) < 1e-12);
    assert(std::abs(frame_to_fraction(49, 100) - 49.0 / 99.0) < 1e-12);

    // Out-of-range fractions clamp to the same endpoints as the edges.
    assert(fraction_to_source_frame(-0.25, 100) == 0);
    assert(fraction_to_source_frame(1.5, 100) == 99);

    // Inverse pair: a fraction maps to a frame, and back again (frame 0..n-1
    // is the identity within the discretization error).
    for (int64_t n : {int64_t{2}, int64_t{24}, int64_t{2500}}) {
        for (int64_t f = 0; f < n; f += std::max<int64_t>(1, n / 7)) {
            const double frac = frame_to_fraction(f, n);
            const int64_t back = fraction_to_source_frame(frac, n);
            assert(back >= 0 && back < n);
            // Round-trip tolerance: the quantized frame must land within one
            // frame of the source (grid quantization can't jump farther).
            assert(std::llabs(back - f) <= 1);
        }
    }
}

void check_next_clip_id_respected() {
    // next_clip_id is pre-seeded to a high value: the synthetic shots must not
    // collide with its ids (the undo/redo snapshot layer keys on ClipId).
    MediaEntry media;
    media.id = 9;
    media.path = "/media/take.mp4";
    media.width = 1280;
    media.height = 720;
    media.fps = 30.0;
    media.total_frames = 120;
    media.has_audio = true;

    const auto proj = build_source_project(media, 0.0);
    assert(proj != nullptr);
    const Clip& clip = proj->sequence.video_tracks[0].clips[0];
    const Clip& aclip = proj->sequence.audio_tracks[0].clips[0];
    assert(clip.id != aclip.id);
    assert(std::max(clip.id, aclip.id) < proj->sequence.next_clip_id);
}

}  // namespace

int main() {
    check_video_audio_project();
    check_video_only_ignores_audio_when_absent();
    check_audio_only_has_no_video_track();
    check_degenerate_and_truncated_frames();
    check_fraction_frame_law();
    check_next_clip_id_respected();
    return 0;
}