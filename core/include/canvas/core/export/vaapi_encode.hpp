#pragma once

// VAAPI encoder rate-control + speed mapping (docs/vaapi.md §7.D5 / §8.1).
//
// libx264/NVENC-style quality is expressed as `crf`; VAAPI encoders have NO
// `crf` option, so pushing it as a private option silently no-ops and the
// export runs at the driver's default rate control (the engine's §8.1 gap).
// The correct VAAPI knobs are:
//   rc_mode           "CQP" (constant quality, the universal VAAPI CRF analog),
//                     "QVBR" (quality-targeted VBR, needs a bitrate), "VBR",
//                     "CBR". "ICQ" is Intel-only — radeonsi h264/hevc_vaapi
//                     reject it ("Driver does not support ICQ RC mode"),
//                     measured on Mesa 26.2.2 gfx1037.
//   global_quality    the quality value (0-51 for H.264/HEVC; AV1 uses a
//                     0-255 scale, so OBS/HW deployments feed crf*5)
//   qp                constant QP (immich's verified command sets both
//                     -qp:v and -global_quality:v)
//   async_depth       encode parallelism; default 2 saturates VCN, higher
//                     gains nothing (measured 1080p: 2->255fps, 64->230fps)
//   quality           h264-only speed/quality trade; higher=slower on radeonsi
//                     (quality=-1 default ~621fps vs 4/7 ~280fps at 640x360)
//
// Pure module: no FFmpeg types in the mapping so it is trivially unit-testable
// without linking libavcodec; the apply() halves are thin av_opt_set wrappers.

#include <cstdint>
#include <string>

// Forward-declared only — this header does not pull in libavcodec. Users that
// actually call apply_vaapi_rate_control() must include <libavcodec/avcodec.h>
// (attributes a full type) like the exporter does.
struct AVCodecContext;

namespace canvas::core {

// True when the FFmpeg encoder name is a VAAPI encoder ("h264_vaapi",
// "hevc_vaapi", "av1_vaapi", ...).
bool is_vaapi_codec(const std::string& codec_name);

// The options that get handed to a VAAPI encoder's priv_data. Defaults keep
// the encoder at its own default behavior; empty fields are simply not set.
struct VaapiRateControl {
    std::string rc_mode;    // "ICQ" | "QVBR" | "CQP" | "VBR" | "CBR" | "" (driver default)
    int global_quality = -1;  // -1 = not set; lower = better
    int qp = -1;              // -1 = not set; constant QP companion to global_quality
};

// Pure mapping — the §8.1 fix. Translates the ExportSettings quality/bitrate
// intent into the VAAPI-specific knob set, so the exporter can stop pushing
// `crf` (a silent no-op on VAAPI encoders) and push the knobs that work.
//
//   codec_name           FFmpeg encoder name (e.g. "h264_vaapi")
//   crf                  -1 = bitrate-driven; else constant quality (0..51)
//   vid_rc_mode          ExportSettings::vid_rc_mode ("auto"/"constqp"/"vbr"/
//                       "vbr_target"/"cbr")
//   video_bitrate_kbps   target bitrate when crf < 0 (unused here otherwise;
//                        the exporter owns the VBV wiring for bitrate modes)
VaapiRateControl vaapi_rate_control_from(const std::string& codec_name, int crf,
                                         const std::string& vid_rc_mode,
                                         int video_bitrate_kbps);

// Applies the mapping to an open-but-not-yet-opened encoder context (call before
// avcodec_open2). Logs a warning if a knob is missing on the selected encoder —
// the D6 rule: an absent private option is a silent no-op, never a hard failure.
void apply_vaapi_rate_control(AVCodecContext* vctx, const VaapiRateControl& rc);

// VAAPI speed knobs derived from an x264-style preset name. 0 / -1 = "leave the
// encoder default" so only meaningful fields are touched.
struct VaapiSpeed {
    int async_depth = 0;  // 0 = encoder default (2); measured: raising it gains nothing
    int quality = -1;     // h264-only ("quality"), -1 = encoder default (fastest)
};

// Pure mapping of the Deliver UI preset name ("placebo".."ultrafast", lowercased
// like nv_preset_for does) into VAAPI speed knobs. Measured law on radeonsi: the
// h264 `quality` option runs INVERTED vs FFmpeg's help text (higher = slower),
// and async_depth leaves VCN speed flat — so the "fast" end of the sweep is
// encoder defaults + tiny async_depth, and only the "slow" end trades speed for
// h264 quality. Unknown names fall back to the tuned default ("faster").
VaapiSpeed vaapi_speed_for(const std::string& codec_name, const std::string& preset);

}  // namespace canvas::core