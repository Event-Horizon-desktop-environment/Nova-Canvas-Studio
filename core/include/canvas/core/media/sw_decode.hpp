#pragma once

// Headless software (CPU) decode + RGBA conversion path for VideoDecoder.
//
// The hardware-decode entry points (decode_to_hw / decode_to_hw_indexed /
// vaapi_export_surface) intentionally stay on VideoDecoder, but everything the
// CPU path does — the sequential forward walk (decode_next /
// decode_forward_to), the seek heuristics (seek_to_frame /
// seek_to_frame_indexed / decode_to_frame), and the swscale RGBA conversion
// (convert_to_rgba, formerly make_rgba_frame) with its frozen-tail hold cache
// and path accounting — lives here so the codec loop and the sws buffer sizing
// can be exercised headlessly without a GPU or media file.
//
// Two cooperating types:
//  - DemuxState  — the shared FFmpeg demux/decode contexts plus the stream
//                  position. Owned by VideoDecoder and borrowed by SoftDecoder
//                  AND the hardware paths, so a software frame and a hardware
//                  frame decode from the same read position without duplicating
//                  or fighting over the AVFormatContext/AVCodecContext. The
//                  shared services (packet pull, container seek, frame clamp,
//                  EOF refinement, keyframe index) live here because both paths
//                  call them.
//  - SoftDecoder — owns the swscale context, the output-dim cap, the frozen-tail
//                  hold, and the sequential/seek path counters, and implements
//                  the decode-loop methods on top of whatever DemuxState the
//                  owner points it at.
//
// This module is deliberately Qt-free (see scripts/check_qtdep.sh under the
// core/include + core/src allowlist).

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

// Fast-over budgets shared by every decode walk (the software seek paths, and
// mirrored by the public VideoDecoder::kPreviewMaxOver/kFullResMaxOver statics):
// a far forward walk decodes at most ~1200 (preview) / ~4000 (full-res) frames
// before returning the nearest decoded frame as an approximate teaser, so a
// sparse-keyframe GOP can never stall the decode worker for tens of seconds.
inline constexpr int kPreviewMaxOver = 1200;
inline constexpr int kFullResMaxOver = 4000;

// One entry in a media file's keyframe (I-frame) index. Built once by walking
// the container packet stream and recording each keyframe's byte position and
// presentation time. Scrubbing uses it to seek straight to the I-frame at-or-
// before the target and know exactly how long the decode-forward tail is — no
// per-seek container search, so random seeks cost one Group of Pictures instead
// of a demuxer scan.
struct IframeEntry {
    int64_t packet_pos = 0;   // file byte offset of the keyframe packet
    double pts_seconds = 0.0; // presentation time (seconds) of the keyframe
    int64_t frame = 0;        // frame number of the keyframe
};

// Process-wide I-frame index cache, keyed by media path. Populated by
// background builder threads, read by decoders on the playback thread. Owned
// here (not on any VideoDecoder/SoftDecoder) so a build that outlives a decoder
// still lands safely in the cache.
inline std::mutex s_iframe_mtx;
inline std::map<std::string, std::shared_ptr<const std::vector<IframeEntry>>> s_iframe_cache;
inline std::set<std::string> s_iframe_inflight;

// Decode-health maintenance counters shared by BOTH the software path
// (sw_decode.cpp) and the hardware path (video_decoder.cpp), so the
// consecutive-failure burst tracker sees one stream of decode results, not two
// independent halves. Defined in sw_decode.cpp.
void decode_fail(const char* where);
void decode_ok();

// Read-stall detector (~1/s): counts av_read_frame calls that blew past the
// 20ms "smooth demux" bound, used by the SW loops here AND the audio decode
// loop in video_decoder.cpp (the audio loop still lives there). Shared (not
// file-static) so the aggregate is one across all readers. Defined in
// sw_decode.cpp.
void read_stall_tick(double ms);

// The shared demux/decode session state. Owned by VideoDecoder, mutated by the
// software path (SoftDecoder) and the hardware path (decode_to_hw*).
struct DemuxState {
    [[nodiscard]] bool is_open() const { return fmt_ctx != nullptr; }

    // Releases the FFmpeg contexts and resets every field. Safe to call on an
    // already-closed state (all pointers are nulled on close).
    void close();
    // Flushes the codec and re-arms the stream position to `resume_frame`
    // (draining cleared). The container position is NOT touched here — use
    // container_seek_seconds() for that.
    void reset_stream(int64_t resume_frame);
    // Clamps `target` into the valid source-frame window [0, last_frame]. When
    // the encoded extent is still unknown (last_frame == -1) the target is only
    // floored at 0; the clamp becomes effective as soon as one frame has been
    // decoded or a stream end is observed. Stops a scrub/play call over the
    // media's end from walking the entire GOP chain to EOF.
    [[nodiscard]] int64_t clamp_target(int64_t target) const;
    // Narrows last_frame to the stream's own encoded extent when the container
    // duration was missing at open(). Uses the video stream duration when
    // present, else the container duration, else the frame-rate fallback.
    void refine_last_frame();
    // Positions the demuxer to `target_seconds` via avformat_seek_file without
    // decoding (used by the indexed seek path), then re-arms the walk.
    void container_seek_seconds(double target_seconds);
    // Reads the next video packet from the container and feeds it to the codec.
    // Returns true when a packet was sent; on EOF or a read error latches
    // draining (and feeds the drain sentinel so a following receive surfaces
    // the EOF that tightens last_frame) and returns false.
    [[nodiscard]] bool read_video_packet();

    [[nodiscard]] bool has_iframe_index() const;
    // Builds the I-frame index on demand (a single packet walk over the stream,
    // on a detached background thread).
    void build_iframe_index();
    // Returns the entry for the I-frame nearest at-or-before `target`, or
    // nullptr if the index isn't built / `target` is before the first I-frame.
    [[nodiscard]] const IframeEntry* iframe_at_or_before(int64_t target) const;

    // ---- shared FFmpeg contexts (owned here, freed in close()) ----
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
    // Resolved per-file color spec (matrix_/range_ start on the codecpar tags
    // and range may be upgraded to Full by the luma probe in open()). Both the
    // software sws conversion and the hardware export path read it.
    gpu::ColorMatrix matrix = gpu::ColorMatrix::BT709;
    gpu::ColorRange range = gpu::ColorRange::Limited;
    int64_t last_frame = -1;
    int64_t next_frame = 0;
    bool draining = false;
    // Media path: keyframe-index cache key and the close-log identity.
    std::string path;
};

// The software decode path (see module comment). Pointed at a DemuxState owned
// elsewhere; the owner must call set_demux() after open and on adoption (move).
class SoftDecoder {
public:
    SoftDecoder() = default;
    void set_demux(DemuxState* demux) { demux_ = demux; }

    // Counter snapshot backing VideoDecoder::PathStats (same fields in the same
    // order). Sequential = frame reached by the cheap forward walk; seeks =
    // (re)seek; convert_ms = time spent hw->cpu transfer + swscale.
    [[nodiscard]] std::uint64_t path_seq() const { return path_seq_; }
    [[nodiscard]] std::uint64_t path_seeks() const { return path_seeks_; }
    [[nodiscard]] double path_seq_ms() const { return path_seq_ms_; }
    [[nodiscard]] double path_seek_ms() const { return path_seek_ms_; }
    [[nodiscard]] double convert_ms() const { return convert_ms_; }
    // Atomically zeroes the SW-side path counters (the HW path's own counters
    // are tracked on VideoDecoder, not here).
    void reset_path_counters();

    // Low-res preview cap for convert_to_rgba (0 = full resolution). Caps the
    // longest output edge so a GOP scrub doesn't build full-res RGBA per frame.
    void set_output_dim(int max_output_dim) {
        out_max_dim_ = max_output_dim > 0 ? max_output_dim : 0;
    }
    [[nodiscard]] int output_dim() const { return out_max_dim_; }

    // Releases the swscale context, the frozen-tail hold, and the counters.
    void close();

    ~SoftDecoder() { close(); }

    // Decodes the next sequential frame and converts it to RGBA. nullptr on
    // EOF/error (and at stream end the highest produced frame is recorded in
    // demux->last_frame so later requests clamp).
    VideoFramePtr decode_next();
    // Decodes forward from the current position until `target`, fast-overs
    // every intermediate frame (no RGBA conversion, no GPU->CPU copy) and
    // converts only the target. `max_over` caps the fast-overs: when exceeded
    // the nearest decoded frame is returned as an approximate teaser instead of
    // walking a multi-second GOP.
    VideoFramePtr decode_forward_to(int64_t target, int max_over = 0);
    // Returns the frame at `target`, sequential-forward when cheap, else seeks.
    VideoFramePtr decode_to_frame(int64_t target_frame, int max_output_dim = 0);
    // Container-seek + decode-forward to `target` (plain FFmpeg seek).
    VideoFramePtr seek_to_frame(int64_t target_frame, int max_output_dim = 0);
    // Frame-accurate seek that first jumps to the owning I-frame from the key
    // index (when available) then decode-forwards within that single GOP.
    VideoFramePtr seek_to_frame_indexed(int64_t target_frame, int max_output_dim = 0);
    // Converts one decoded source frame (CPU, or GPU-then-downloaded) to an
    // owning RGBA VideoFrame, honoring the preview-dim cap and the resolved
    // color spec. This is the sws buffer-sizing seam (issue #4): the destination
    // is sized with av_image_get_buffer_size(align=32) and written through
    // av_image_fill_arrays, never by a hand-derived w*4*h guess.
    VideoFramePtr convert_to_rgba(const AVFrame* src, int64_t ticks, double seconds,
                                  int64_t number);

    // Movable: the swscale context, hold, and counters transfer; the source's
    // demux_ pointer is nulled so the moved-from SoftDecoder never reaches into
    // the receiver's (now owned elsewhere) DemuxState.
    SoftDecoder(SoftDecoder&& o) noexcept;
    SoftDecoder& operator=(SoftDecoder&& o) noexcept;
    SoftDecoder(const SoftDecoder&) = delete;
    SoftDecoder& operator=(const SoftDecoder&) = delete;

private:
    DemuxState* demux_ = nullptr;
    SwsContext* sws_ctx_ = nullptr;
    int out_max_dim_ = 0;
    // Frozen-tail hold (software path): the last real frame, served on any later
    // past-end request instead of re-seeking + re-decoding the identical final
    // frame (~90ms/frame) for the whole audio-only share of a project.
    VideoFramePtr hold_rgba_;
    int64_t hold_rgba_src_ = -1;
    int hold_rgba_dim_ = -1;
    // Sequential-walk vs keyframe-seek bookkeeping (see path_seq()).
    std::uint64_t path_seq_ = 0;
    std::uint64_t path_seeks_ = 0;
    double path_seq_ms_ = 0.0;
    double path_seek_ms_ = 0.0;
    double convert_ms_ = 0.0;
};

}  // namespace canvas::core