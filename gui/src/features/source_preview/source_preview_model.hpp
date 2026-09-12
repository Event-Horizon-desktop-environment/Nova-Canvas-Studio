#pragma once

// Headless source-preview model (Dual Viewer + media-pool scrub, splitplan
// "own folder + own cpp/hpp" rule). Qt-free by design: the shared playback
// stack (SequenceController/TimelineDecoder/AudioPipeline) previews a media
// pool entry by building a synthetic single-clip project around it, and the
// scrub/playhead laws below map between that project's frames and UI
// geometry. Kept headless so this file compiles in the Qt-free test targets
// (a stray <Q...> include breaks that link on purpose).
//
// A headless module may include ONLY <system>, <canvas/core/...>, and other
// headless modules — never <Q...>.

#include <cstdint>
#include <memory>

#include "canvas/core/project/project.hpp"

namespace canvas::gui::source_preview {

// Builds a synthetic single-clip project around `media` so the shared playback
// stack can preview it exactly like a timeline clip:
//   - sequence fps  = media fps, else `fallback_fps` (still <= 0 ⇒ 30)
//   - one video track with Clip{media.id, tl_in 0 .. tl_out total_frames,
//     src_in 0 .. src_out total_frames} whenever the media carries video
//     (width/height > 0)
//   - one audio track with a linked clip whenever the media carries audio,
//     so AudioPipeline mixes real audio while previewing.
// `has_audio`/`has_video` drive the audio mixer and viewer mode decisions; all
// frame math is int64_t `>= 0` (tl_out is end-exclusive, matching the model).
[[nodiscard]] std::shared_ptr<const canvas::core::Project> build_source_project(
    const canvas::core::MediaEntry& media, double fallback_fps);

// Scrub fraction <-> source frame law shared by the pool-tile hover, the
// SourceViewerPanel mini-scrub slider, and the controller. `fraction` is
// clamped to [0,1]; the identity maps fraction 0 -> frame 0 and fraction 1 ->
// last frame (total_frames - 1). A degenerate total (<= 1) maps everything to
// frame 0.
[[nodiscard]] int64_t fraction_to_source_frame(double fraction, int64_t total_frames);
[[nodiscard]] double frame_to_fraction(int64_t frame, int64_t total_frames);

}  // namespace canvas::gui::source_preview