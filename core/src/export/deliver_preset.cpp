#include "canvas/core/export/deliver_preset.hpp"

#include "canvas/core/media/hw_device.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <sstream>

namespace canvas::core {

namespace {

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

const char* backend_device(EncoderBackend b) {
    switch (b) {
        case EncoderBackend::NVIDIA: return "cuda";
        case EncoderBackend::AMD:    return "vaapi";
        case EncoderBackend::Intel:  return "qsv";
        case EncoderBackend::Auto:
        case EncoderBackend::CPU:
        default: return "";
    }
}

}

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

bool scope_is_still(const RenderScope scope) noexcept {
    return scope == RenderScope::Still;
}

bool scope_is_sequence(const RenderScope scope) noexcept {
    return scope == RenderScope::FrameSequence;
}

bool scope_is_image(const RenderScope scope) noexcept {
    return scope_is_still(scope) || scope_is_sequence(scope);
}

namespace {

std::string image_stem(const std::string& base) {
    const std::size_t slash = base.find_last_of("/\\");
    const std::size_t dot = base.find_last_of('.');
    if (dot != std::string::npos && (slash == std::string::npos || dot > slash))
        return base.substr(0, dot);
    return base;
}

}

std::string still_output_path(const std::string& base) {
    return image_stem(base) + ".png";
}

std::string sequence_output_path(const std::string& base, const int64_t frame_index) {
    char buf[32];
    const long long idx = frame_index < 1 ? 1 : static_cast<long long>(frame_index);
    std::snprintf(buf, sizeof(buf), "%05lld", idx);
    return image_stem(base) + "_" + buf + ".png";
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

    const std::string& pg = HwDeviceManager::preferred_gpu_backend();
    if (!pg.empty()) {
        const std::string mine = backend_device(backend);
        if (!mine.empty() && mine != pg) {
            if (sw_fallback) *sw_fallback = true;
            backend = EncoderBackend::CPU;
        }
    }

    switch (codec) {
        case VideoCodec::H264: {
            if (backend == EncoderBackend::NVIDIA) return "h264_nvenc";
            if (backend == EncoderBackend::AMD) return "h264_vaapi";
            if (backend == EncoderBackend::Intel) return "h264_qsv";
            if (backend == EncoderBackend::Auto) {
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

    es.width = ds.video.custom_width;
    es.height = ds.video.custom_height;
    es.fps = ds.video.custom_fps;

    bool sw_fallback = false;
    VideoCodec vc = video_codec_from_string(ds.video.codec);
    EncoderBackend backend = ds.video.encoder;
    if (backend == EncoderBackend::Auto) {
        backend = EncoderBackend::CPU;
    }
    es.video_codec = video_encoder_name(vc, backend, es.format, &sw_fallback);

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
    es.threads = ds.advanced.threads;

    switch (ds.video.rate_control) {
        case RateControl::ConstantQP:
            es.crf = ds.video.quality;
            es.video_bitrate_kbps = 0;
            es.vid_rc_mode = "constqp";
            break;
        case RateControl::VBRQuality:
            es.crf = ds.video.quality;
            es.video_bitrate_kbps = 0;
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
    if (ds.video.rate_control == RateControl::VBRTargetKbps ||
        ds.video.rate_control == RateControl::ConstantBitrate)
        es.video_max_bitrate_kbps = ds.video.max_bitrate_kbps;

    std::ostringstream extra;
    extra << "aq-strength=" << ds.video.aq_strength << "\n";
    if (ds.video.lookahead_frames > 0)
        extra << "rc-lookahead=" << ds.video.lookahead_frames << "\n";
    if (ds.video.enable_b_frames()) {
        extra << "b_adapt=1\n";
    }
    if (ds.video.two_pass) {
        extra << "flags=+pass2\n";
    }
    if (ds.video.tuning == EncoderTuning::Lossless)
        extra << "lossless=1\n";
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

}
