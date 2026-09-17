#pragma once

#include <cstddef>
#include <vector>

#include "canvas/core/timeline/model.hpp"

namespace canvas::gui {

struct AudioTarget {
    canvas::core::Track::Kind kind = canvas::core::Track::Kind::Audio;
    std::size_t track = 0;
    canvas::core::ClipId id = 0;
    canvas::core::Clip clip;
};

std::vector<AudioTarget> resolve_audio_targets(
    const canvas::core::Sequence& seq,
    const std::vector<canvas::core::ClipId>& ids);

}
