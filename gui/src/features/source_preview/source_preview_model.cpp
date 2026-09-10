#include "features/source_preview/source_preview_model.hpp"

#include <algorithm>
#include <cmath>
#include <string>

#include "canvas/core/timeline/model.hpp"

namespace canvas::gui::source_preview {

std::shared_ptr<const canvas::core::Project> build_source_project(
    const canvas::core::MediaEntry& media, const double fallback_fps) {
    auto project = std::make_shared<canvas::core::Project>();
    project->name = "source preview";
    project->media.push_back(media);

    double fps = media.fps > 0.0 ? media.fps : fallback_fps;
    if (fps <= 0.0) fps = 30.0;
    project->sequence.fps = fps;

    // A single whole-media clip on a fresh track; tl_out/src_out are
    // end-exclusive, so covering `total_frames` frames spans [0, total_frames).
    const int64_t frames = media.total_frames > 0 ? media.total_frames : 1;
    canvas::core::ClipId next_id = project->sequence.next_clip_id;

    if (media.width > 0 && media.height > 0) {
        canvas::core::Track video;
        video.kind = canvas::core::Track::Kind::Video;
        video.name = "V1";
        canvas::core::Clip clip;
        clip.id = next_id++;
        clip.media = media.id;
        clip.tl_in = 0;
        clip.tl_out = frames;
        clip.src_in = 0;
        clip.src_out = frames;
        video.clips.push_back(std::move(clip));
        project->sequence.video_tracks.push_back(std::move(video));
    }

    if (media.has_audio) {
        canvas::core::Track audio;
        audio.kind = canvas::core::Track::Kind::Audio;
        audio.name = "A1";
        canvas::core::Clip clip;
        clip.id = next_id++;
        clip.media = media.id;
        clip.tl_in = 0;
        clip.tl_out = frames;
        clip.src_in = 0;
        clip.src_out = frames;
        // Link the pair so the A/V mate moves as one during preview playback.
        if (!project->sequence.video_tracks.empty() &&
            !project->sequence.video_tracks.front().clips.empty()) {
            const canvas::core::ClipId video_id =
                project->sequence.video_tracks.front().clips.front().id;
            clip.linked_id = video_id;
            project->sequence.video_tracks.front().clips.front().linked_id = clip.id;
        }
        audio.clips.push_back(std::move(clip));
        project->sequence.audio_tracks.push_back(std::move(audio));
    }

    project->sequence.next_clip_id = next_id;
    return project;
}

int64_t fraction_to_source_frame(const double fraction, const int64_t total_frames) {
    if (total_frames <= 1) return 0;
    const double f = std::clamp(fraction, 0.0, 1.0);
    return std::llround(f * static_cast<double>(total_frames - 1));
}

double frame_to_fraction(const int64_t frame, const int64_t total_frames) {
    if (total_frames <= 1) return 0.0;
    const int64_t f = std::clamp(frame, int64_t{0}, total_frames - 1);
    return static_cast<double>(f) / static_cast<double>(total_frames - 1);
}

}  // namespace canvas::gui::source_preview