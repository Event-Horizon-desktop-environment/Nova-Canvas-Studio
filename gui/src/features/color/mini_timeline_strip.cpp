#include "features/color/mini_timeline_strip.hpp"

#include <QDebug>
#include <QFontMetrics>
#include <QImage>
#include <QMouseEvent>
#include <QPainter>
#include <QPixmap>
#include <QPolygonF>
#include <QResizeEvent>

#include <algorithm>

#include "UX/theme.hpp"
#include "canvas/core/timeline/model.hpp"
#include "features/thumbnails/thumbnail_service.hpp"

namespace canvas::gui {

namespace {

constexpr int kStripMargin = 6;
constexpr int kBoxGap = 5;
constexpr int kMaxBoxHeight = 200;

QPixmap scaled_fill(const QImage& img, int w, int h) {
    if (w <= 0 || h <= 0 || img.isNull()) return QPixmap();
    const double src_aspect = static_cast<double>(img.width()) / img.height();
    const double target_aspect = static_cast<double>(w) / h;
    QRect crop;
    if (src_aspect > target_aspect) {
        const int cw = static_cast<int>(img.height() * target_aspect);
        crop = QRect((img.width() - cw) / 2, 0, cw, img.height());
    } else {
        const int ch = static_cast<int>(img.width() / target_aspect);
        crop = QRect(0, (img.height() - ch) / 2, img.width(), ch);
    }
    const QImage clipped = img.copy(crop);
    if (clipped.isNull()) return QPixmap();
    return QPixmap::fromImage(clipped.scaled(w, h, Qt::IgnoreAspectRatio, Qt::SmoothTransformation));
}

}  // namespace

MiniTimelineStrip::MiniTimelineStrip(QWidget* parent) : QWidget(parent) {
    setMinimumHeight(40);
    setMaximumHeight(220);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    setCursor(Qt::PointingHandCursor);
}

void MiniTimelineStrip::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    if (event->size() == previous_size_) return;
    previous_size_ = event->size();
    qWarning().nospace()
        << "[ministrip] size=" << event->size().width() << "x" << event->size().height()
        << " min_height=" << minimumHeight();
    request_clip_thumbnails();
}

void MiniTimelineStrip::set_sequence(const canvas::core::Sequence* sequence) {
    sequence_ = sequence;
    thumbs_.clear();
    request_clip_.clear();
    request_clip_thumbnails();
    update();
}

void MiniTimelineStrip::set_playhead(int64_t frame) {
    playhead_ = frame;
    update();
}

void MiniTimelineStrip::set_media_paths(
    std::unordered_map<canvas::core::MediaId, MiniMediaMeta> paths) {
    media_paths_ = std::move(paths);
    thumbs_.clear();
    request_clip_.clear();
    request_clip_thumbnails();
    update();
}

void MiniTimelineStrip::set_thumbnail_service(ThumbnailService* service) {
    thumbnail_service_ = service;
    if (service) {
        connect(service, &ThumbnailService::thumbnail_ready, this,
                &MiniTimelineStrip::on_thumbnail_ready);
    }
    request_clip_thumbnails();
}

void MiniTimelineStrip::request_clip_thumbnails() {
    if (!thumbnail_service_ || media_paths_.empty() || !sequence_) return;
    const canvas::core::Track* strip_track = nullptr;
    for (const auto& track : sequence_->video_tracks) {
        if (!track.clips.empty()) strip_track = &track;
    }
    if (!strip_track) return;

    const int n = static_cast<int>(strip_track->clips.size());
    if (n <= 0) return;
    const int body_h = std::max(16, static_cast<int>(height()) - kStripMargin * 2);
    const int box_h = std::min(kMaxBoxHeight, body_h);
    const double box_w = box_h * 16.0 / 9.0;
    const double total_w = static_cast<double>(width()) - kStripMargin * 2;
    const double gaps = kBoxGap * (n - 1);
    const double max_box = (total_w - gaps) / n;
    double draw_w = box_w;
    if (max_box < draw_w) draw_w = max_box;
    const int target_w = std::max(48, static_cast<int>(std::lround(draw_w * 2.0)));
    const int target_h = std::max(32, static_cast<int>(std::lround(box_h * 2.0)));

    for (const auto& clip : strip_track->clips) {
        if (thumbs_.count(clip.id) != 0) continue;
        const auto mit = media_paths_.find(clip.media);
        if (mit == media_paths_.end()) continue;
        const int64_t dur = std::max<int64_t>(1, clip.src_out - clip.src_in);
        const uint64_t req_id = next_request_id_++;
        if (request_clip_.count(req_id) != 0) continue;
        request_clip_[req_id] = clip.id;
        ThumbRequest req;
        req.id = req_id;
        req.path = mit->second.path;
        req.frame = clip.src_in + dur / 2;
        req.target_width = target_w;
        req.max_height = target_h;
        thumbnail_service_->request(req);
    }
}

int64_t MiniTimelineStrip::total_frames() const {
    if (!sequence_) return 0;
    int64_t total = 0;
    for (const auto& track : sequence_->video_tracks) {
        for (const auto& clip : track.clips) {
            total = std::max(total, static_cast<int64_t>(clip.tl_out));
        }
    }
    for (const auto& track : sequence_->audio_tracks) {
        for (const auto& clip : track.clips) {
            total = std::max(total, static_cast<int64_t>(clip.tl_out));
        }
    }
    return std::max<int64_t>(total, 1);
}

namespace {

// Shared geometry helper: computes the box layout the strip uses for painting
// and hit-testing. Returns false when there is nothing to draw.
bool strip_box_layout(const canvas::core::Sequence* seq, int width, int height,
                      int& n, int& bw, int& bh, int& top, double& box_w,
                      int64_t& first_in, int64_t& last_out) {
    n = 0;
    first_in = 0;
    last_out = 0;
    const canvas::core::Track* track = nullptr;
    if (seq) {
        for (const auto& t : seq->video_tracks) {
            if (!t.clips.empty()) track = &t;
        }
    }
    if (!track) return false;
    n = static_cast<int>(track->clips.size());
    if (n <= 0) return false;
    first_in = track->clips.front().tl_in;
    last_out = track->clips.back().tl_out;
    const int body_h = std::max(16, height - kStripMargin * 2);
    const int box_h = std::min(kMaxBoxHeight, body_h);
    box_w = box_h * 16.0 / 9.0;
    const double total_w = static_cast<double>(width) - kStripMargin * 2;
    const double gaps = kBoxGap * (n - 1);
    double draw_w = box_w;
    const double max_box = (total_w - gaps) / n;
    double scale = 1.0;
    if (max_box < draw_w) {
        scale = max_box / draw_w;
        draw_w = max_box;
    }
    bw = static_cast<int>(std::lround(draw_w));
    bh = static_cast<int>(std::lround(box_h * scale));
    top = (height - bh) / 2;
    return true;
}

}  // namespace

bool MiniTimelineStrip::scrub_frame_at(const QPointF& pos, int64_t& clip_owner,
                                       int64_t& out_frame) const {
    clip_owner = -1;
    int n = 0, bw = 0, bh = 0, top = 0;
    double box_w = 0.0;
    int64_t first_in = 0, last_out = 0;
    if (!strip_box_layout(sequence_, width(), height(), n, bw, bh, top, box_w,
                          first_in, last_out))
        return false;
    if (pos.y() < top || pos.y() > top + bh) return false;

    const double xp = pos.x();
    double x = kStripMargin;
    const canvas::core::Track* track = nullptr;
    for (const auto& t : sequence_->video_tracks) {
        if (!t.clips.empty()) track = &t;
    }
    for (const auto& clip : track->clips) {
        if (xp >= x && xp <= x + bw) {
            const double t = std::clamp((xp - x) / static_cast<double>(bw), 0.0, 1.0);
            out_frame = clip.tl_in +
                static_cast<int64_t>(std::llround(t * (clip.tl_out - clip.tl_in)));
            clip_owner = clip.id;
            return true;
        }
        x += bw + kBoxGap;
    }

    // A gap, or beyond the last box: scrub across the whole clip span so the
    // playhead still tracks the pointer.
    const double span_w = static_cast<double>(width()) - kStripMargin * 2;
    const double t = std::clamp((xp - kStripMargin) / span_w, 0.0, 1.0);
    out_frame = first_in + static_cast<int64_t>(std::llround(t * (last_out - first_in)));
    return true;
}

double MiniTimelineStrip::frame_to_strip_x(int64_t frame) const {
    int n = 0, bw = 0, bh = 0, top = 0;
    double box_w = 0.0;
    int64_t first_in = 0, last_out = 0;
    if (!strip_box_layout(sequence_, width(), height(), n, bw, bh, top, box_w,
                          first_in, last_out))
        return kStripMargin;

    const canvas::core::Track* track = nullptr;
    for (const auto& t : sequence_->video_tracks) {
        if (!t.clips.empty()) track = &t;
    }
    if (!track) return kStripMargin;

    double x = kStripMargin;
    const auto& clips = track->clips;
    const auto it = std::find_if(clips.begin(), clips.end(), [frame](const auto& c) {
        return frame >= c.tl_in && frame <= c.tl_out;
    });
    if (it != clips.end()) {
        const auto idx = static_cast<std::size_t>(std::distance(clips.begin(), it));
        x += static_cast<double>(idx) * (bw + kBoxGap);
        const int64_t dur = std::max<int64_t>(1, it->tl_out - it->tl_in);
        const double t = std::clamp(
            static_cast<double>(frame - it->tl_in) / dur, 0.0, 1.0);
        return x + t * bw;
    }

    // Outside any clip: proportional to the whole span.
    const double span_w = static_cast<double>(width()) - kStripMargin * 2;
    const double t = std::clamp(
        static_cast<double>(frame - first_in) / std::max<int64_t>(1, last_out - first_in),
        0.0, 1.0);
    return kStripMargin + t * span_w;
}

void MiniTimelineStrip::on_thumbnail_ready(uint64_t request_id, const QImage& image) {
    if (image.isNull()) return;
    const auto it = request_clip_.find(request_id);
    if (it == request_clip_.end()) return;
    thumbs_[it->second] = image;
    request_clip_.erase(it);
    update();
}

void MiniTimelineStrip::paintEvent(QPaintEvent* event) {
    Q_UNUSED(event);
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const ThemeTokens& t = tokens();

    if (!sequence_ || sequence_->video_tracks.empty()) {
        p.setPen(with_alpha(t.ink, 60));
        p.drawText(rect(), Qt::AlignCenter, tr("No sequence"));
        return;
    }

    // Last video track wins the strip (top-most on-screen clip content).
    const canvas::core::Track* strip_track = nullptr;
    for (const auto& track : sequence_->video_tracks) {
        if (!track.clips.empty()) strip_track = &track;
    }
    if (!strip_track) {
        p.setPen(with_alpha(t.ink, 60));
        p.drawText(rect(), Qt::AlignCenter, tr("No video clips"));
        return;
    }

    // 16:9 boxes, sized to fit all clips.
    const int body_h = std::max(16, static_cast<int>(height()) - kStripMargin * 2);
    const int box_h = std::min(kMaxBoxHeight, body_h);
    const double box_w = box_h * 16.0 / 9.0;
    const int n = static_cast<int>(strip_track->clips.size());
    if (n <= 0) return;

    const double total_w = static_cast<double>(width()) - kStripMargin * 2;
    const double gaps = kBoxGap * (n - 1);
    double draw_w = box_w;
    const double max_box = (total_w - gaps) / n;
    double scale = 1.0;
    if (max_box < draw_w) {
        scale = max_box / draw_w;
        draw_w = max_box;
    }
    const int bh = static_cast<int>(std::lround(box_h * scale));
    const int bw = static_cast<int>(std::lround(draw_w));
    const int top = (height() - bh) / 2;

    double x = kStripMargin;
    int idx = 0;
    for (const auto& clip : strip_track->clips) {
        const QRectF rect(x, top, bw, bh);
        QImage thumb;
        const auto tit = thumbs_.find(clip.id);
        if (tit != thumbs_.end()) thumb = tit->second;

        if (!thumb.isNull()) {
            p.drawPixmap(rect.toAlignedRect(), scaled_fill(thumb, bw, bh));
        } else {
            QColor fill = t.clip_video;
            if (clip.enabled) {
                const double h = 0.62 - 0.05 * (idx % 5);
                fill = QColor::fromHsvF(h, 0.34, 0.42);
            }
            p.setPen(QPen(t.border, 1.0));
            p.setBrush(fill);
            p.drawRoundedRect(rect, 3.0, 3.0);
        }

        p.setPen(QPen(t.border, 1.0));
        p.setBrush(Qt::NoBrush);
        p.drawRect(rect.adjusted(0.5, 0.5, -0.5, -0.5));

        x += bw + kBoxGap;
        ++idx;
    }

    // Playhead over the clip that currently owns the playhead frame.
    int64_t ph_frame = playhead_;
    const canvas::core::Clip* active = nullptr;
    for (const auto& clip : strip_track->clips) {
        if (ph_frame >= clip.tl_in && ph_frame < clip.tl_out) {
            active = &clip;
            break;
        }
    }

    // Playhead line follows the actual playhead frame (drags when scrubbing).
    const double phx = frame_to_strip_x(playhead_);
    const QPointF ph(phx, top - 2.0);
    p.setPen(QPen(t.playhead, 1.8));
    p.drawLine(ph, QPointF(phx, top + bh + 2.0));
    p.setBrush(t.playhead);
    p.setPen(Qt::NoPen);
    p.drawPolygon(QPolygonF{ph - QPointF(4.0, 0.0), ph + QPointF(4.0, 0.0),
                            ph - QPointF(0.0, 4.0)});
}

void MiniTimelineStrip::mousePressEvent(QMouseEvent* event) {
    if (event->button() != Qt::LeftButton) return;
    if (!sequence_) return;
    int64_t owner = -1, frame = 0;
    if (!scrub_frame_at(event->position(), owner, frame)) return;
    scrubbing_ = true;
    scrub_owner_clip_ = owner;
    playhead_ = frame;
    update();
    emit scrub_begin();
    emit scrubbed(frame);
    if (owner >= 0) emit clip_activated(owner, frame);
}

void MiniTimelineStrip::mouseMoveEvent(QMouseEvent* event) {
    if (!scrubbing_) {
        QWidget::mouseMoveEvent(event);
        return;
    }
    int64_t owner = -1, frame = 0;
    if (!scrub_frame_at(event->position(), owner, frame)) return;
    scrub_owner_clip_ = owner;
    playhead_ = frame;
    update();
    emit scrubbed(frame);
}

void MiniTimelineStrip::mouseReleaseEvent(QMouseEvent* event) {
    if (!scrubbing_) {
        QWidget::mouseReleaseEvent(event);
        return;
    }
    scrubbing_ = false;
    int64_t owner = -1, frame = 0;
    if (scrub_frame_at(event->position(), owner, frame)) {
        playhead_ = frame;
        update();
        emit scrubbed(frame);
        emit scrub_committed(frame);
    }
    QWidget::mouseReleaseEvent(event);
}

}  // namespace canvas::gui