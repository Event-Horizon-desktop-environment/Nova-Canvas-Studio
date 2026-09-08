#pragma once

// DeliverSettingsModel — Qt-free policy for the Deliver settings panel
// (splitplan Phase 31).
//
// Qt-free on purpose: this module wraps canvas::core's deliver list functions
// and supplies the container<->codec restriction policy + bitrate-control
// visibility the panel applies to its combo boxes, so it can be exercised from
// the headless test seam (gui/tests, linked without Qt) and scanned by
// scripts/check_qtdep.sh. It never includes a <Q...> header; DeliverSettingsPanel
// keeps the widget wiring and delegates the list/visibility decisions here.
//
// All codec/encoder names are the display strings from canvas::core's
// deliver_*() helpers, so combo population and settings() round-trips stay
// byte-identical.

#include <string>
#include <vector>

namespace canvas::gui {
namespace deliver_model {

// The fixed deliver presets shown in the header combo (display names). The
// selected name is stored into DeliverSettings::preset_name; applying actual
// preset parameter defaults on selection is not implemented yet (the combo
// records the name and the rest of the panel keeps its current values).
const std::vector<std::string>& preset_names();

// Encoder backend choices (Auto/CPU/NVIDIA/AMD/Intel) — passthrough wrapper
// over canvas::core::deliver_encoders().
std::vector<std::string> encoder_backends();

// Video codecs allowed in a given container form; restricted to combos the
// FFmpeg muxer accepts (WebM+H.264 and MP4+ProRes are rejected up front). The
// container is the display name from canvas::core::deliver_formats() ("MKV",
// "MP4", "QuickTime", "WebM", ...); matching is case-insensitive. Codecs are the
// display names from canvas::core::deliver_video_codecs().
std::vector<std::string> video_codecs_for_format(const std::string& format);

// Audio codecs allowed in a given container (WebM cannot carry AAC/MP3, MXF is
// PCM-only, ...). Same naming + case rules as the video list.
std::vector<std::string> audio_codecs_for_format(const std::string& format);

// Which bitrate fields the rate-control mode shows, and what the single "bit
// rate" field is labelled. `rate_control_index` is the integer value of
// canvas::core::RateControl (0=ConstantQP, 1=VBRQuality, 2=VBRTargetKbps,
// 3=ConstantBitrate).
struct BitrateVisibility {
    bool show_bitrate = false;      // single "Bit Rate" / "Target (Kbps)" field
    bool show_max = false;          // "Max (Kbps)" field
    const char* bitrate_label = "Bit Rate";  // VBR-target relabels to "Target (Kbps)"
};
BitrateVisibility bitrate_visibility(int rate_control_index);

}  // namespace deliver_model
}  // namespace canvas::gui