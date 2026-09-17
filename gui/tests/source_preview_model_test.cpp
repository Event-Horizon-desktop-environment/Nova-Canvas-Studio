#include <cmath>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>

#include "canvas/core/project/project.hpp"
#include "canvas/core/timeline/model.hpp"
#include "features/source_preview/source_preview_model.hpp"

namespace {

int g_failures = 0;
#define CHECK(cond)                                                        \
    do {                                                                   \
        if (!(cond)) {                                                     \
            std::printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);  \
            ++g_failures;                                                  \
        }                                                                  \
    } while (0)

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
    CHECK(proj != nullptr);
    CHECK(proj->media.size() == 1);
    CHECK(proj->media[0].id == 3);
    CHECK(proj->sequence.video_tracks.size() == 1);
    CHECK(proj->sequence.audio_tracks.size() == 1);
    CHECK(proj->sequence.fps == 25.0);

    const Track& vt = proj->sequence.video_tracks[0];
    CHECK(vt.kind == Track::Kind::Video);
    CHECK(vt.clips.size() == 1);
    const Clip& clip = vt.clips[0];
    CHECK(clip.media == 3);
    CHECK(clip.tl_in == 0);
    CHECK(clip.tl_out == 2500);
    CHECK(clip.src_in == 0);
    CHECK(clip.src_out == 2500);
    CHECK(clip.linked_id != 0);

    const Track& at = proj->sequence.audio_tracks[0];
    CHECK(at.kind == Track::Kind::Audio);
    CHECK(at.clips.size() == 1);
    const Clip& aclip = at.clips[0];
    CHECK(aclip.media == 3);
    CHECK(aclip.tl_in == 0);
    CHECK(aclip.tl_out == 2500);
    CHECK(aclip.linked_id == clip.id);

    CHECK(proj->sequence.duration_frames() == 2500);

    CHECK(proj->sequence.next_clip_id > clip.id);
}

void check_video_only_ignores_audio_when_absent() {
    MediaEntry media;
    media.id = 1;
    media.path = "/media/video.mp4";
    media.fps = 0.0;
    media.width = 640;
    media.height = 360;
    media.total_frames = 500;
    media.has_audio = false;

    const auto proj = build_source_project(media, 24.0);
    CHECK(proj != nullptr);
    CHECK(proj->sequence.video_tracks.size() == 1);
    CHECK(proj->sequence.audio_tracks.empty());
    CHECK(proj->sequence.fps == 24.0);
    CHECK(proj->sequence.video_tracks[0].clips.size() == 1);
    CHECK(proj->sequence.video_tracks[0].clips[0].tl_out == 500);
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
    CHECK(proj != nullptr);
    CHECK(proj->sequence.video_tracks.empty());
    CHECK(proj->sequence.audio_tracks.size() == 1);
    CHECK(proj->sequence.fps == 48.0);
    const Clip& clip = proj->sequence.audio_tracks[0].clips[0];
    CHECK(clip.tl_out == 4800);
    CHECK(clip.linked_id == 0);
}

void check_degenerate_and_truncated_frames() {
    MediaEntry media;
    media.id = 2;
    media.path = "/media/still.jpg";
    media.width = 800;
    media.height = 600;
    media.total_frames = -1;
    media.has_audio = false;

    const auto proj = build_source_project(media, 0.0);
    CHECK(proj != nullptr);
    CHECK(proj->sequence.fps == 30.0);
    const Clip& clip = proj->sequence.video_tracks[0].clips[0];
    CHECK(clip.tl_out == 1);
    CHECK(clip.src_out == 1);

    CHECK(fraction_to_source_frame(0.0, 1) == 0);
    CHECK(fraction_to_source_frame(0.5, 1) == 0);
    CHECK(fraction_to_source_frame(1.0, 1) == 0);
    CHECK(frame_to_fraction(0, 1) == 0.0);
    CHECK(frame_to_fraction(42, 1) == 0.0);
    CHECK(frame_to_fraction(0, 1) == 0.0);
}

void check_fraction_frame_law() {
    CHECK(fraction_to_source_frame(0.0, 100) == 0);
    CHECK(fraction_to_source_frame(1.0, 100) == 99);
    CHECK(fraction_to_source_frame(0.5, 100) == 50);
    CHECK(frame_to_fraction(0, 100) == 0.0);
    CHECK(std::abs(frame_to_fraction(99, 100) - 1.0) < 1e-12);
    CHECK(std::abs(frame_to_fraction(49, 100) - 49.0 / 99.0) < 1e-12);

    CHECK(fraction_to_source_frame(-0.25, 100) == 0);
    CHECK(fraction_to_source_frame(1.5, 100) == 99);

    for (int64_t n : {int64_t{2}, int64_t{24}, int64_t{2500}}) {
        for (int64_t f = 0; f < n; f += std::max<int64_t>(1, n / 7)) {
            const double frac = frame_to_fraction(f, n);
            const int64_t back = fraction_to_source_frame(frac, n);
            CHECK(back >= 0 && back < n);
            CHECK(std::llabs(back - f) <= 1);
        }
    }
}

void check_next_clip_id_respected() {
    MediaEntry media;
    media.id = 9;
    media.path = "/media/take.mp4";
    media.width = 1280;
    media.height = 720;
    media.fps = 30.0;
    media.total_frames = 120;
    media.has_audio = true;

    const auto proj = build_source_project(media, 0.0);
    CHECK(proj != nullptr);
    const Clip& clip = proj->sequence.video_tracks[0].clips[0];
    const Clip& aclip = proj->sequence.audio_tracks[0].clips[0];
    CHECK(clip.id != aclip.id);
    CHECK(std::max(clip.id, aclip.id) < proj->sequence.next_clip_id);
}

}

int main() {
    check_video_audio_project();
    check_video_only_ignores_audio_when_absent();
    check_audio_only_has_no_video_track();
    check_degenerate_and_truncated_frames();
    check_fraction_frame_law();
    check_next_clip_id_respected();
    if (g_failures) {
        std::printf("%d source-preview check(s) FAILED\n", g_failures);
        return 1;
    }
    std::printf("source_preview_model_test PASS\n");
    return 0;
}
