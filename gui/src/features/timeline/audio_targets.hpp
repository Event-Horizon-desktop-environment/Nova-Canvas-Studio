#pragma once

// Qt-free audio-target resolution for mixer edits (Phase 4). The Inspector's
// Volume/Pan controls must drive EVERY audio clip the current selection owns:
// an audio clip in the selection directly, or the linked audio mate of a
// selected video clip. This module turns a selection (a flat ClipId list) plus
// the Sequence into the concrete per-clip edit targets, de-duplicating linked
// A/V pairs so one applied value lands once per actual audio clip.
//
// Headless seam: only <cstddef>/<vector> + core headers. Registered under
// scripts/check_qtdep.sh and exercised by gui/tests/audio_targets_test.

#include <cstddef>
#include <vector>

#include "canvas/core/timeline/model.hpp"

namespace canvas::gui {

// One audio clip a mixer edit should drive. `kind` is always Audio and `track`
// indexes Sequence::audio_tracks; `id` is the AUDIO clip's id (a selected video
// clip resolves to its mate's id, so edits target the clip that actually has
// audio). `clip` is a full copy (volume/pan/pitch/EQ) so the Inspector can
// populate all of its controls from the first target.
struct AudioTarget {
    canvas::core::Track::Kind kind = canvas::core::Track::Kind::Audio;
    std::size_t track = 0;
    canvas::core::ClipId id = 0;
    canvas::core::Clip clip;
};

// Resolve every audio clip the selection can drive. For each id in `ids`, in
// order: an audio clip yields itself; a video clip with a linked audio mate
// yields that mate; a video clip with no audio is skipped. Linked pairs appear
// once (the mate already satisfies the pair), so the output is ready to loop in
// apply commits without extra de-dup. Empty when nothing in the selection is
// audio.
std::vector<AudioTarget> resolve_audio_targets(
    const canvas::core::Sequence& seq,
    const std::vector<canvas::core::ClipId>& ids);

}  // namespace canvas::gui