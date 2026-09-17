#pragma once

#include "canvas/core/gpu/colorspace.hpp"
#include "canvas/core/media/frame.hpp"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/hwcontext.h>
#include <libavutil/rational.h>
#include <libswscale/swscale.h>
}

#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <vector>

namespace canvas::core {

inline constexpr int kPreviewMaxOver = 1200;
inline constexpr int kFullResMaxOver = 4000;

struct IframeEntry {
    int64_t packet_pos = 0;
    double pts_seconds = 0.0;
    int64_t frame = 0;
};

inline std::mutex s_iframe_mtx;
inline std::map<std::string, std::shared_ptr<const std::vector<IframeEntry>>> s_iframe_cache;
inline std::set<std::string> s_iframe_inflight;

void decode_fail(const char* where);
void decode_ok();

void read_stall_tick(double ms);

struct DemuxState {
    [[nodiscard]] bool is_open() const { return fmt_ctx != nullptr; }

    void close();
    void reset_stream(int64_t resume_frame);
    [[nodiscard]] int64_t clamp_target(int64_t target) const;
    void refine_last_frame();
    void container_seek_seconds(double target_seconds);
    [[nodiscard]] bool read_video_packet();

    [[nodiscard]] bool has_iframe_index() const;
    void build_iframe_index();
    [[nodiscard]] const IframeEntry* iframe_at_or_before(int64_t target) const;

    AVFormatContext* fmt_ctx = nullptr;
    AVCodecContext* codec_ctx = nullptr;
    AVFrame* av_frame = nullptr;
    AVPacket* packet = nullptr;
    int hw_pix_fmt = AV_PIX_FMT_NONE;
    bool hw_avail = false;
    int video_stream = -1;
    AVRational stream_tb{0, 1};
    int width = 0;
    int height = 0;
    double frame_rate = 0.0;
    double duration_seconds = 0.0;
    int64_t total_frames = -1;
    gpu::ColorMatrix matrix = gpu::ColorMatrix::BT709;
    gpu::ColorRange range = gpu::ColorRange::Limited;
    int64_t last_frame = -1;
    int64_t next_frame = 0;
    bool draining = false;
    std::string path;
};

class SoftDecoder {
public:
    SoftDecoder() = default;
    void set_demux(DemuxState* demux) { demux_ = demux; }

    [[nodiscard]] std::uint64_t path_seq() const { return path_seq_; }
    [[nodiscard]] std::uint64_t path_seeks() const { return path_seeks_; }
    [[nodiscard]] double path_seq_ms() const { return path_seq_ms_; }
    [[nodiscard]] double path_seek_ms() const { return path_seek_ms_; }
    [[nodiscard]] double convert_ms() const { return convert_ms_; }
    void reset_path_counters();

    void set_output_dim(int max_output_dim) {
        out_max_dim_ = max_output_dim > 0 ? max_output_dim : 0;
    }
    [[nodiscard]] int output_dim() const { return out_max_dim_; }

    void close();

    ~SoftDecoder() { close(); }

    VideoFramePtr decode_next();
    VideoFramePtr decode_forward_to(int64_t target, int max_over = 0);
    VideoFramePtr decode_to_frame(int64_t target_frame, int max_output_dim = 0);
    VideoFramePtr seek_to_frame(int64_t target_frame, int max_output_dim = 0);
    VideoFramePtr seek_to_frame_indexed(int64_t target_frame, int max_output_dim = 0);
    VideoFramePtr convert_to_rgba(const AVFrame* src, int64_t ticks, double seconds,
                                  int64_t number);

    SoftDecoder(SoftDecoder&& o) noexcept;
    SoftDecoder& operator=(SoftDecoder&& o) noexcept;
    SoftDecoder(const SoftDecoder&) = delete;
    SoftDecoder& operator=(const SoftDecoder&) = delete;

private:
    DemuxState* demux_ = nullptr;
    SwsContext* sws_ctx_ = nullptr;
    int out_max_dim_ = 0;
    VideoFramePtr hold_rgba_;
    int64_t hold_rgba_src_ = -1;
    int hold_rgba_dim_ = -1;
    std::uint64_t path_seq_ = 0;
    std::uint64_t path_seeks_ = 0;
    double path_seq_ms_ = 0.0;
    double path_seek_ms_ = 0.0;
    double convert_ms_ = 0.0;
};

}
