#include "canvas/core/export/deliver_preset.hpp"

#include <algorithm>
#include <cctype>
#include <sstream>

namespace canvas::core {

namespace {

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

std::string trim(std::string s) {
    const auto not_space = [](unsigned char c) { return !std::isspace(c); };
    s.erase(s.begin(), std::find_if(s.begin(), s.end(), not_space));
    s.erase(std::find_if(s.rbegin(), s.rend(), not_space).base(), s.end());
    return s;
}

const char* ffmpeg_suffix(EncoderBackend b) {
    switch (b) {
        case EncoderBackend::NVIDIA: return "_nvenc";
        case EncoderBackend::AMD:    return "_vaapi";
        case EncoderBackend::Intel:  return "_qsv";
        case EncoderBackend::Auto:
        case EncoderBackend::CPU:
        default: return "";
    }
}

}  // namespace

std::string container_format_name(const std::string& format) {
    const std::string f = lower(format);
    if (f == "mkv" || f == "matroska") return "matroska";
    if (f == "mp4" || f == "mpeg-4") return "mp4";
    if (f == "mov" || f == "quicktime" || f == "quicktime movie") return "mov";
    if (f == "webm") return "webm";
    if (f == "avi") return "avi";
    if (f == "ogg") return "ogv";
    if (f == "mxf") return "mxf";
    if (f == "mxf op-atom") return "mxf_opatom";
    if (f == "mxf op1a") return "mxf";
    if (f == "gif") return "gif";
    if (f == "dpx") return "dpx";
    if (f == "exr") return "exr";
    if (f == "png") return "image2";
    if (f == "tiff") return "image2";
    if (f == "jpeg" || f == "jpg") return "image2";
    if (f == "webp") return "webp";
    if (f == "mp3") return "mp3";
    if (f == "wav") return "wav";
    if (f == "flac") return "flac";
    if (f == "itm" || f == "imf") return "imf";
    if (f == "mpeg-2" || f == "mpeg2") return "mpeg";
    return f;
}

VideoCodec video_codec_from_string(const std::string& codec) {
    const std::string c = lower(codec);
    std::string compact;
    compact.reserve(c.size());
    for (char ch : c)
        if (ch != '.' && ch != ' ')
            compact += ch;
    if (compact.find("h264") != std::string::npos || c == "avc") return VideoCodec::H264;
    if (compact.find("h265") != std::string::npos || compact.find("hevc") != std::string::npos)
        return VideoCodec::H265;
    if (compact.find("av1") != std::string::npos) return VideoCodec::AV1;
    if (compact.find("prores") != std::string::npos) return VideoCodec::ProRes;
    if (compact.find("ffv1") != std::string::npos) return VideoCodec::FFV1;
    if (compact.find("jpeg") != std::string::npos || compact.find("j2k") != std::string::npos)
        return VideoCodec::JPEG2000;
    if (compact.find("raw") != std::string::npos || compact.find("uncompressed") != std::string::npos)
        return VideoCodec::Uncompressed;
    return VideoCodec::H265;
}

std::string video_encoder_name(VideoCodec codec, EncoderBackend backend,
                               const std::string& container_format, bool* sw_fallback) {
    if (sw_fallback) *sw_fallback = false;

    switch (codec) {
        case VideoCodec::H264: {
            if (backend == EncoderBackend::NVIDIA) return "h264_nvenc";
            if (backend == EncoderBackend::AMD) return "h264_vaapi";
            if (backend == EncoderBackend::Intel) return "h264_qsv";
            if (backend == EncoderBackend::Auto) {
                // Auto: prefer hardware if present, but keep it simple: NVIDIA
                // NVENC is the default when available. The GUI probes and can
                // pass Backend::NVIDIA explicitly; here we default to CPU-safe.
                if (sw_fallback) *sw_fallback = true;
                return "libx264";
            }
            return "libx264";
        }
        case VideoCodec::H265: {
            if (backend == EncoderBackend::NVIDIA) return "hevc_nvenc";
            if (backend == EncoderBackend::AMD) return "hevc_vaapi";
            if (backend == EncoderBackend::Intel) return "hevc_qsv";
            if (backend == EncoderBackend::Auto) {
                if (sw_fallback) *sw_fallback = true;
                return "libx265";
            }
            return "libx265";
        }
        case VideoCodec::AV1: {
            if (backend == EncoderBackend::NVIDIA) return "av1_nvenc";
            if (backend == EncoderBackend::AMD) return "av1_vaapi";
            if (backend == EncoderBackend::Intel) return "av1_qsv";
            if (backend == EncoderBackend::Auto) {
                if (sw_fallback) *sw_fallback = true;
                return "libsvtav1";
            }
            return "libsvtav1";
        }
        case VideoCodec::ProRes: {
            if (sw_fallback) *sw_fallback = true;
            return "prores_ks";
        }
        case VideoCodec::FFV1: {
            if (sw_fallback) *sw_fallback = true;
            return "ffv1";
        }
        case VideoCodec::JPEG2000: {
            if (sw_fallback) *sw_fallback = true;
            return "jpeg2000";
        }
        case VideoCodec::Uncompressed: {
            // Raw uyvy/rgb in a container; fall back to a lossless-ish path.
            const std::string c = lower(container_format);
            if (c == "mov") return "rawvideo";
            if (sw_fallback) *sw_fallback = true;
            return "rawvideo";
        }
    }
    if (sw_fallback) *sw_fallback = true;
    return "libx264";
}

ExportSettings to_export_settings(const DeliverSettings& ds) {
    ExportSettings es;

    const bool was_hw = ds.video.encoder == EncoderBackend::NVIDIA ||
                        ds.video.encoder == EncoderBackend::AMD ||
                        ds.video.encoder == EncoderBackend::Intel;
    (void)was_hw;

    es.format = container_format_name(ds.video.format);
    es.audio_codec = ds.audio.export_audio ? lower(ds.audio.codec) : "";
    es.audio_bitrate_kbps = ds.audio.bitrate_kbps;
    es.audio_sample_rate = ds.audio.sample_rate;
    es.audio_channels = ds.audio.channels;
    es.remove_audio = !ds.audio.export_audio;

    // Width/height from resolution choice (FPS/resolution resolved by the GUI
    // before calling; defaults are applied here as a fallback).
    es.width = ds.video.custom_width;
    es.height = ds.video.custom_height;
    es.fps = ds.video.custom_fps;

    // Encoder selection.
    bool sw_fallback = false;
    VideoCodec vc = video_codec_from_string(ds.video.codec);
    EncoderBackend backend = ds.video.encoder;
    if (backend == EncoderBackend::Auto) {
        // If the codec supports a hardware encoder on this machine, prefer it;
        // otherwise the CPU is used. We choose NVIDIA when present (common case).
        // The GUI passes the resolved backend after probing; here we keep CPU.
        backend = EncoderBackend::CPU;
    }
    es.video_codec = video_encoder_name(vc, backend, es.format, &sw_fallback);

    // Preset values are codec-specific. x264/x265 and the hardware families
    // (nvenc/qsv/vaapi/amf) share a compatible set of speed names; AV1 software
    // encoders (SVT-AV1/libaom) use a numeric scale. The exporter translates the
    // UI's x264-style name to the right per-family preset via nv_preset_for(), so
    // forward it verbatim here and let the exporter map it. Other encoders
    // (ProRes, FFV1, JPEG 2000, rawvideo, ...) either reject them or use a
    // completely different scale, so skip the preset there.
    const std::string& encn = es.video_codec;
    const bool has_preset = encn.find("x264") != std::string::npos ||
                            encn.find("x265") != std::string::npos ||
                            encn.find("nvenc") != std::string::npos ||
                            encn.find("qsv") != std::string::npos ||
                            encn.find("vaapi") != std::string::npos ||
                            encn.find("amf") != std::string::npos ||
                            encn.find("av1") != std::string::npos ||
                            encn.find("svt") != std::string::npos;
    es.preset = has_preset ? lower(ds.video.preset) : "";

    // Rate control mapping.
    switch (ds.video.rate_control) {
        case RateControl::ConstantQP:
            es.crf = ds.video.quality;
            es.video_bitrate_kbps = 0;  // quality-driven: never force a bitrate
            es.vid_rc_mode = "constqp";
            break;
        case RateControl::VBRQuality:
            es.crf = ds.video.quality;
            es.video_bitrate_kbps = 0;  // quality-driven (crf-based)
            es.vid_rc_mode = "constqp";
            break;
        case RateControl::VBRTargetKbps:
            es.crf = -1;
            es.video_bitrate_kbps = ds.video.target_bitrate_kbps;
            es.vid_rc_mode = "vbr_target";
            break;
        case RateControl::ConstantBitrate:
            es.crf = -1;
            es.video_bitrate_kbps = ds.video.target_bitrate_kbps;
            es.vid_rc_mode = "cbr";
            break;
    }
    // The max bitrate ceiling (VBV buffer bound) applies to every bitrate-driven
    // mode. It is what constrains each frame's size so the stream actually lands
    // near the target instead of running away in hard-to-encode regions. When it
    // equals the target (the UI default) this yields true CBR. Not applied to
    // quality-driven modes, which ignore bitrate entirely.
    if (ds.video.rate_control == RateControl::VBRTargetKbps ||
        ds.video.rate_control == RateControl::ConstantBitrate)
        es.video_max_bitrate_kbps = ds.video.max_bitrate_kbps;

    // Tuning / quality knobs pushed through as extra options for encoders that
    // expose them (best-effort; unknown opts are ignored by libav).
    std::ostringstream extra;
    if (ds.video.aq_strength > 0)
        extra << "aq-strength=" << ds.video.aq_strength << "\n";
    if (ds.video.lookahead_frames > 0)
        extra << "rc-lookahead=" << ds.video.lookahead_frames << "\n";
    if (ds.video.enable_b_frames()) {
        // Resolve sends `b_adapt=1` (adaptive B-frame decision) here, NOT `bf=N`.
        // Our exporter keeps AVCodecContext.max_b_frames = 0 (the frame pipeline
        // feeds frames in display order, so reordering is disabled); pushing `bf=2`
        // against that makes NVENC fail with "invalid param (8)". Emit Resolve's
        // b_adapt option instead, which is a no-op when the encoder has no B-frames.
        extra << "b_adapt=1\n";
    }
    if (ds.video.two_pass) {
        // libvpx-style 2-pass uses pass=1/2; x264 uses x264opts. Keep minimal.
        extra << "flags=+pass2\n";
    }
    if (ds.video.tuning == EncoderTuning::Lossless)
        extra << "lossless=1\n";
    // Note: Resolve renders with NVENC tuning "ultra_quality" (-tune uhq), but
    // through FFmpeg's option surface `tune=uhq` combined with CBR rate control,
    // rc-lookahead and split-encode makes the encoder reject the config
    // (InitializeEncoder failed: invalid param). The FFmpeg NVENC wrapper can't
    // reproduce the exact SDL-level `ultra_quality` tuning Resolve drives, so we
    // intentionally don't emit `tune=uhq`; the speed-preset parity (faster->p2)
    // is what actually reproduces Resolve's render throughput.
    extra << ds.advanced.extra_options;
    es.extra = extra.str();

    return es;
}

std::vector<std::string> deliver_formats() {
    return {"MKV", "MP4", "QuickTime", "WebM", "AVI", "DPX", "EXR", "GIF", "JPEG",
            "JPEG 2000", "PNG", "TIFF", "WebP", "MXF OP-Atom", "MXF OP1A", "MPEG-2"};
}

std::vector<std::string> deliver_video_codecs() {
    return {"H.264", "H.265", "AV1", "Apple ProRes", "FFV1", "JPEG 2000", "Uncompressed"};
}

std::vector<std::string> deliver_audio_codecs() {
    return {"AAC", "MP3", "PCM", "FLAC", "Opus", "Vorbis"};
}

std::vector<std::string> deliver_encoders() {
    return {"Auto", "CPU", "NVIDIA", "AMD", "Intel"};
}

}  // namespace canvas::core
