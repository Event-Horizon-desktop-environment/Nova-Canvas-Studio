#pragma once

// Mini-timeline strip (design spec §Layout-5): a compact cosmetic overview of
// the sequence shown above the Color workspace. Reads the project's sequence
// and paints each clip on the topmost video track as a 16:9 thumbnail box
// (requested via ThumbnailService) plus a live playhead. Clicking a box seeks
// the playhead to that clip (Milestone 0 UX scaffold — no grade data).

#include <QWidget>

#include <chrono>
#include <cstdint>
#include <string>
#include <unordered_map>

#include "canvas/core/timeline/model.hpp"

namespace canvas::core {
struct Sequence;
}

namespace canvas::gui {

class ThumbnailService;

// Thumbnail request-id namespace for the color-page mini strip. Shares the one
// app-wide ThumbnailService with the timeline filmstrip, media pool, project
// manager and source preview; each consumer owns a disjoint high bit so
// completion handlers can never match another consumer's decode. The strip and
// the timeline used to both count from ~0/1, guaranteeing a collision over time
// ("filmstrip never fully populated" / wrong frames). Bit 59 is below every
// existing namespace region (pool = bit63, project = bits61-63,
// source-preview = bits60-63 + bit0, timeline = bit60).
inline constexpr std::uint64_t kMiniStripThumbNs = 0x0800000000000000ULL;
// O(1) gate on_thumbnail_ready uses to reject ids posted by other consumers.
inline bool is_mini_strip_thumb_id(std::uint64_t id) noexcept {
    return (id & kMiniStripThumbNs) != 0;
}

// Lightweight media metadata for looking up the source path behind a clip's
// media id so the strip can request a thumbnail frame.
struct MiniMediaMeta {
    std::string path;
    int64_t total_frames = 0;
    double fps = 0.0;
};

class MiniTimelineStrip : public QWidget {
    Q_OBJECT
public:
    explicit MiniTimelineStrip(QWidget* parent = nullptr);

    void set_sequence(const canvas::core::Sequence* sequence);
    void set_playhead(int64_t frame);
    void set_media_paths(std::unordered_map<canvas::core::MediaId, MiniMediaMeta> paths);
    void set_thumbnail_service(ThumbnailService* service);

    [[nodiscard]] const canvas::core::Sequence* sequence() const { return sequence_; }

signals:
    void clip_activated(canvas::core::ClipId id, int64_t timeline_frame);
    // Drag-to-scrub lifecycle: scrub_begin fires when a drag grab starts,
    // scrubbed on every pointer move (live preview), scrub_committed once on
    // release.
    void scrub_begin();
    void scrubbed(int64_t timeline_frame);
    void scrub_committed(int64_t timeline_frame);

private slots:
    void on_thumbnail_ready(uint64_t request_id, const QImage& image);

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;

private:
    bool scrub_frame_at(const QPointF& pos, int64_t& clip_owner,
                        int64_t& out_frame) const;
    [[nodiscard]] double frame_to_strip_x(int64_t frame) const;
    void request_clip_thumbnails();
    [[nodiscard]] int64_t total_frames() const;
    const canvas::core::Sequence* sequence_ = nullptr;
    int64_t playhead_ = 0;
    QSize previous_size_;
    bool scrubbing_ = false;
    int64_t scrub_owner_clip_ = -1;
    // Drag-to-scrub move taps fire at pointer-move rate; peers (sequence seek
    // previews) already throttle, so gate the per-move trace line here too.
    std::chrono::steady_clock::time_point last_scrub_log_{};
    std::unordered_map<canvas::core::MediaId, MiniMediaMeta> media_paths_;
    ThumbnailService* thumbnail_service_ = nullptr;
    uint64_t next_request_id_ = kMiniStripThumbNs;
    std::unordered_map<uint64_t, canvas::core::ClipId> request_clip_;
    std::unordered_map<canvas::core::ClipId, QImage> thumbs_;
};

}  // namespace canvas::gui