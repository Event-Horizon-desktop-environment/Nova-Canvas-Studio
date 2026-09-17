#pragma once

// EDL (CMX3600) export LAW (Qt-free). Produces a classic CMX3600 edit decision
// list from a sequence: a `TITLE:`/`FCM: NON-DROP FRAME` header and one event
// per clip, sorted by timeline in-point. This is the interchange format most
// NLEs (Resolve, Premiere, Avid, Kdenlive) import, so it is the cheapest way to
// move an edit out of Nova Canvas without an XML/AAF implementation.
//
// Formatting only — no file I/O. The GUI/deliver layer decides where to write.

#include "canvas/core/project/project.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace canvas::core::edl {

// HH:MM:SS:FF non-drop timecode for `frame` at integer `fps` (fps <= 0 → 30).
[[nodiscard]] std::string timecode(int64_t frame, int fps);

// Full CMX3600 document. `media` supplies the reel names (the clip's media file
// basename, uppercased + truncated to the 8-char CMX reel field); a title clip
// (media < 0) or an unknown media id uses the `AX` auxiliary reel. `title`
// fills the `TITLE:` line.
[[nodiscard]] std::string write_cmx3600(const Sequence& seq, const std::string& title,
                                        const std::vector<MediaEntry>& media);

}  // namespace canvas::core::edl