#pragma once

#include "canvas/core/media/frame.hpp"
#include "canvas/core/media/vaapi/surface.hpp"
#include "canvas/core/media/sw_decode.hpp"

extern "C" {
#include <libavutil/hwcontext.h>
#include <libavutil/rational.h>
#include <libswresample/swresample.h>
}

#include <cstdint>
#include <memory>
#include <string>

namespace canvas::core {

class VideoDecoder {
public:
    VideoDecoder() = default;
    ~VideoDecoder();
    VideoDecoder(VideoDecoder&& o) noexcept;
    VideoDecoder& operator=(VideoDecoder&& o) noexcept;
    VideoDecoder(const VideoDecoder&) = delete;
    VideoDecoder& operator=(const VideoDecoder&) = delete;

    bool open(const std::string& path, std::string* error = nullptr,
              const AVBufferRef* hw_device_ctx = nullptr,
              const char* gpu_label = nullptr);
    void close();

    [[nodiscard]] bool is_open() const { return demux_state_ && demux_state_->fmt_ctx != nullptr; }
    [[nodiscard]] bool is_hardware() const { return hw_avail_; }
    [[nodiscard]] const char* hardware_name() const { return hw_avail_ ? hw_type_name_.c_str() : "sw"; }
    [[nodiscard]] const char* gpu_label() const { return hw_gpu_label_.c_str(); }
    [[nodiscard]] int width() const { return demux_state_ ? demux_state_->width : 0; }
    [[nodiscard]] int height() const { return demux_state_ ? demux_state_->height : 0; }
    [[nodiscard]] int nb_streams() const {
        return demux_state_ && demux_state_->fmt_ctx ? demux_state_->fmt_ctx->nb_streams : 0;
    }
    [[nodiscard]] int video_stream_index() const {
        return demux_state_ ? demux_state_->video_stream : -1;
    }
    [[nodiscard]] bool is_still_picture() const;
    [[nodiscard]] std::string video_stream_summary() const;
    [[nodiscard]] double frame_rate() const { return demux_state_ ? demux_state_->frame_rate : 0.0; }
    [[nodiscard]] double duration_seconds() const {
        return demux_state_ ? demux_state_->duration_seconds : 0.0;
    }
    [[nodiscard]] int64_t total_frames() const {
        return demux_state_ ? demux_state_->total_frames : -1;
    }
    [[nodiscard]] int64_t current_frame() const {
        return demux_state_ ? demux_state_->next_frame : 0;
    }
    [[nodiscard]] gpu::ColorSpec color_spec() const {
        return demux_state_ ? gpu::ColorSpec{demux_state_->matrix, demux_state_->range}
                            : gpu::ColorSpec{gpu::ColorMatrix::BT709, gpu::ColorRange::Limited};
    }
    [[nodiscard]] int64_t last_frame() const {
        return demux_state_ ? demux_state_->last_frame : -1;
    }

    VideoFramePtr decode_next();
    void set_output_dim(int max_output_dim);

    struct PathStats {
        std::uint64_t sequential = 0;
        std::uint64_t seeks = 0;
        double sequential_ms = 0.0;
        double seek_ms = 0.0;
        double convert_ms = 0.0;
    };
    [[nodiscard]] PathStats path_stats() const;
    PathStats take_path_stats();

    VideoFramePtr decode_to_frame(int64_t target_frame, int max_output_dim = 0);
    VideoFramePtr seek_to_frame(int64_t target_frame, int max_output_dim = 0);

    [[nodiscard]] bool has_iframe_index() const {
        return demux_state_ && demux_state_->has_iframe_index();
    }
    void build_iframe_index();
    [[nodiscard]] const IframeEntry* iframe_at_or_before(int64_t target) const;
    VideoFramePtr seek_to_frame_indexed(int64_t target, int max_output_dim = 0);

    const AVFrame* decode_to_hw(int64_t target_frame, int max_over = 0);

    const AVFrame* decode_to_hw_indexed(int64_t target_frame, int max_over = 0);

    static const int kPreviewMaxOver = canvas::core::kPreviewMaxOver;

    [[nodiscard]] vaapi::VaapiSurfacePtr vaapi_export_surface(const AVFrame* hw,
                                                              int64_t frame_number) const;

    static const int kFullResMaxOver = canvas::core::kFullResMaxOver;

    [[nodiscard]] bool has_audio() const { return audio_stream_ >= 0; }
    [[nodiscard]] int audio_sample_rate() const { return audio_sample_rate_; }
    [[nodiscard]] int audio_channels() const { return audio_channels_; }
    AudioChunkPtr decode_audio(int64_t start_sample, int max_frames, int out_sample_rate);
    void seek_audio(int64_t start_sample, int sample_rate);

private:
    std::unique_ptr<DemuxState> demux_state_;
    SoftDecoder sw_decoder_;
    int hw_pix_fmt_ = AV_PIX_FMT_NONE;
    bool hw_avail_ = false;
    std::string hw_type_name_;
    std::string hw_gpu_label_;
    bool hw_engaged_ = false;
    bool soft_only_ = false;

    AVFrame* hold_hw_ = nullptr;
    int64_t hold_hw_src_ = -1;
    AVFrame* retain_hw_ = nullptr;
    int64_t retain_hw_src_ = -1;

    int audio_stream_ = -1;
    AVFormatContext* audio_fmt_ctx_ = nullptr;
    AVCodecContext* audio_codec_ = nullptr;
    SwrContext* swr_ctx_ = nullptr;
    AVFrame* audio_frame_ = nullptr;
    AVPacket* audio_packet_ = nullptr;
    int audio_sample_rate_ = 0;
    int audio_channels_ = 0;
    int64_t audio_next_sample_ = 0;
    int64_t audio_samples_total_ = 0;
    AVRational audio_tb_{0, 1};
};

}
