#pragma once

#include <cstddef>
#include <cstdint>

namespace canvas::gui {

inline constexpr std::size_t kLookahead = 24;

inline constexpr std::size_t kScrubPrecache = 4;

inline constexpr int kPreviewMaxDim = 640;

inline constexpr std::int64_t kCommitSeqMaxDelta = 96;

inline constexpr int kAudioLeadMs = 120;

}
