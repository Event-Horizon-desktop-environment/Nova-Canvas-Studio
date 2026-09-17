#pragma once

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

inline constexpr std::uint64_t kMiniStripThumbNs = 0x0800000000000000ULL;
inline bool is_mini_strip_thumb_id(std::uint64_t id) noexcept {
    return (id & kMiniStripThumbNs) != 0;
}

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
    std::chrono::steady_clock::time_point last_scrub_log_{};
    std::unordered_map<canvas::core::MediaId, MiniMediaMeta> media_paths_;
    ThumbnailService* thumbnail_service_ = nullptr;
    uint64_t next_request_id_ = kMiniStripThumbNs;
    std::unordered_map<uint64_t, canvas::core::ClipId> request_clip_;
    std::unordered_map<canvas::core::ClipId, QImage> thumbs_;
};

}
