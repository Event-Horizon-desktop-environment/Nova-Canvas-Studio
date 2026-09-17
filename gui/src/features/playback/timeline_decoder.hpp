#pragma once

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include "canvas/core/grade_graph/lut.hpp"
#include "canvas/core/media/frame.hpp"
#include "canvas/core/media/frame_cache.hpp"
#include "canvas/core/media/hw_device.hpp"
#include "canvas/core/media/video_decoder.hpp"
#include "canvas/core/project/project.hpp"

namespace canvas::gui {

class TimelineDecoder {
public:
    void add_media(const canvas::core::MediaEntry& entry);
    void close();
    void invalidate(canvas::core::MediaId media);

    canvas::core::VideoFramePtr decode(const canvas::core::Project& project,
                                   const canvas::core::Clip& clip,
                                   std::int64_t seq_frame, int max_dim = 0);
    canvas::core::Nv12FramePtr decode_nv12(const canvas::core::Project& project,
                                       const canvas::core::Clip& clip,
                                       std::int64_t seq_frame, int max_dim);
    canvas::core::VideoFramePtr make_black_frame(const canvas::core::Clip& clip,
                                             int max_dim = 0) const;

    canvas::core::RenderFramePtr frame(const canvas::core::Project& project,
                                   std::int64_t seq_frame);
    canvas::core::RenderFramePtr preview(const canvas::core::Project& project,
                                     std::int64_t seq_frame, int max_dim);
    struct PreviewStats {
        std::uint64_t hits = 0;
        std::uint64_t misses = 0;
        std::uint64_t evictions = 0;
    };
    [[nodiscard]] PreviewStats take_preview_stats();
    struct GradeStats {
        std::uint64_t samples = 0;
        double ms_sum = 0.0;
        double ms_max = 0.0;
    };
    [[nodiscard]] GradeStats take_grade_stats();
    double media_rate_at(const canvas::core::Project& project, std::int64_t seq_frame,
                         double fallback_fps) const;

    [[nodiscard]] const canvas::core::HwDeviceManager& hw() const noexcept { return hw_; }

    [[nodiscard]] bool is_loaded(canvas::core::MediaId id) const;
    [[nodiscard]] bool is_hardware(canvas::core::MediaId id) const;

    struct TransitionBakeCandidate {
        std::int64_t win_start = -1;
        std::int64_t win_end = -1;
    };
    [[nodiscard]] std::optional<TransitionBakeCandidate>
    next_transition_bake_candidate(const canvas::core::Project& project,
                                   std::int64_t seq_frame) const;

private:
    struct DecoderSlot {
        canvas::core::VideoDecoder decoder;
        canvas::core::FrameCache cache;
        bool loaded = false;
    };

    struct PreviewKey {
        canvas::core::MediaId media = 0;
        std::int64_t frame = 0;
        bool operator==(const PreviewKey& o) const noexcept {
            return media == o.media && frame == o.frame;
        }
    };
    struct PreviewKeyHash {
        std::size_t operator()(const PreviewKey& k) const noexcept {
            std::size_t h = static_cast<std::size_t>(k.media) * 0x9E3779B97F4A7C15ULL;
            h ^= static_cast<std::size_t>(k.frame) * 0x9E3779B97F4A7C15ULL;
            return h;
        }
    };

    std::unordered_map<canvas::core::MediaId, std::unique_ptr<DecoderSlot>> slots_;
    std::unordered_map<canvas::core::MediaId, std::unique_ptr<DecoderSlot>> b_slots_;
    std::unordered_map<PreviewKey, canvas::core::VideoFramePtr, PreviewKeyHash> preview_cache_;
    std::deque<PreviewKey> preview_lru_;
    static constexpr std::size_t kPreviewCacheMax = 32;
    std::uint64_t preview_hits_ = 0;
    std::uint64_t preview_misses_ = 0;
    std::uint64_t preview_evictions_ = 0;

    canvas::core::HwDeviceManager hw_{"playback"};

    const canvas::core::Clip* top_video_clip_at(const canvas::core::Project& project,
                                            std::int64_t seq_frame) const;

    const canvas::core::Clip* media_clip_beneath(const canvas::core::Project& project,
                                                 std::int64_t seq_frame) const;

    canvas::core::Nv12FramePtr decode_nv12_slot(DecoderSlot* slot,
                                                const canvas::core::Project& project,
                                                const canvas::core::Clip& clip,
                                                std::int64_t seq_frame, int max_dim);

    void open_b_slot(const canvas::core::Project& project,
                     const canvas::core::Clip& clip);

    canvas::core::grade_graph::GradeLutPtr grade_lut_for(const canvas::core::Clip& clip);

    void attach_title_transition(canvas::core::RenderFrame& out,
                                 const canvas::core::Project& project,
                                 const canvas::core::Clip& a,
                                 std::int64_t seq_frame);

    std::uint64_t grade_samples_ = 0;
    double grade_ms_sum_ = 0.0;
    double grade_ms_max_ = 0.0;
    struct GradeLutKey {
        canvas::core::ClipId clip_id = 0;
        std::uint64_t change_seq = 0;
        bool operator==(const GradeLutKey&) const = default;
    };
    struct GradeLutKeyHash {
        std::size_t operator()(const GradeLutKey& k) const noexcept {
            return std::hash<std::uint64_t>()(
                       static_cast<std::uint64_t>(k.clip_id) ^
                       (k.change_seq * 0x9E3779B97F4A7C15ull)) ^
                   std::hash<std::uint64_t>()(k.change_seq);
        }
    };
    std::unordered_map<GradeLutKey, canvas::core::grade_graph::GradeLutPtr,
                       GradeLutKeyHash>
        grade_lut_cache_;

    struct TransitionBakeJob {
        canvas::core::MediaEntry a_entry;
        canvas::core::MediaEntry b_entry;
        double seq_fps = 0.0;
        canvas::core::Clip a;
        canvas::core::Clip b;
        std::int64_t win_start = 0;
        std::int64_t win_end = 0;
    };
    struct BakedTransition {
        canvas::core::ClipId a_id = 0;
        canvas::core::MediaId b_media = -1;
        std::int64_t win_start = 0;
        std::int64_t win_end = 0;
        std::vector<std::pair<canvas::core::Nv12FramePtr, canvas::core::Nv12FramePtr>> planes;
        std::unique_ptr<canvas::core::VideoDecoder> parked_b;
        std::int64_t served = 0;
    };

    bool transition_bake_candidate(const canvas::core::Project& project,
                                   std::int64_t seq_frame,
                                   TransitionBakeJob* out) const;
    void maybe_start_transition_bake(const canvas::core::Project& project,
                                     std::int64_t seq_frame);
    void adopt_or_clear_transition_bake(std::int64_t seq_frame);
    void transition_bake_thread();
    void run_transition_bake(const TransitionBakeJob& job);

    static constexpr std::int64_t kTransitionBakeLead = 96;
    static constexpr std::int64_t kTransitionBakeMaxFrames = 16;

    std::mutex bake_mutex_;
    std::condition_variable bake_cv_;
    std::thread bake_thread_;
    std::unique_ptr<TransitionBakeJob> bake_job_;
    bool bake_inflight_ = false;
    bool bake_stop_ = false;
    std::unique_ptr<BakedTransition> bake_result_;
};

}
