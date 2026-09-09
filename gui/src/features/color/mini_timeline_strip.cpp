#include "features/color/mini_timeline_strip.hpp"

#include <QFont>
#include <QFontMetrics>
#include <QMouseEvent>
#include <QPainter>
#include <QPolygonF>

#include <algorithm>

#include "UX/theme.hpp"
#include "canvas/core/timeline/model.hpp"

namespace canvas::gui {

MiniTimelineStrip::MiniTimelineStrip(QWidget* parent) : QWidget(parent) {
    setMinimumHeight(52);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    setCursor(Qt::PointingHandCursor);
}

void MiniTimelineStrip::set_sequence(const canvas::core::Sequence* sequence) {
    sequence_ = sequence;
    update();
}

void MiniTimelineStrip::set_playhead(int64_t frame) {
    playhead_ = frame;
    update();
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

void MiniTimelineStrip::paintEvent(QPaintEvent* event) {
    Q_UNUSED(event);
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const ThemeTokens& t = tokens();

    const QRectF video_row(8.0, 6.0, width() - 16.0, 24.0);
    const QRectF audio_row(8.0, 34.0, width() - 16.0, 12.0);

    if (!sequence_ || sequence_->video_tracks.empty()) {
        p.setPen(with_alpha(t.ink, 60));
        p.drawText(rect(), Qt::AlignCenter, tr("No sequence"));
        return;
    }

    const int64_t total = total_frames();
    const double px_per_frame = video_row.width() / double(total);

    // Last video track wins the filmstrip row (top-most on-screen clip content).
    const canvas::core::Track* strip_track = nullptr;
    for (const auto& track : sequence_->video_tracks) {
        if (!track.clips.empty()) strip_track = &track;
    }

    if (strip_track) {
        int idx = 0;
        for (const auto& clip : strip_track->clips) {
            const double x0 = video_row.left() + clip.tl_in * px_per_frame;
            const double x1 = video_row.left() + clip.tl_out * px_per_frame;
            const QRectF cell(std::max(x0, video_row.left()),
                              video_row.top(), std::max(2.0, x1 - x0),
                              video_row.height());

            QColor fill = t.clip_video;
            if (clip.enabled) {
                const double h = 0.62 - 0.05 * (idx % 5);
                fill = QColor::fromHsvF(h, 0.34, 0.42);
            }
            p.setPen(QPen(t.border, 1.0));
            p.setBrush(fill);
            p.drawRoundedRect(cell, 3.0, 3.0);

            if (cell.width() > 34.0) {
                // Index chip + trimmed name + src timecode.
                p.setPen(with_alpha(t.ink, 150));
                p.setFont(QFont(font().family(), 8, QFont::Bold));
                const QFontMetrics fm = p.fontMetrics();
                const QString idx_text = QString::number(idx + 1);
                const QRectF chip(cell.left() + 3.0, cell.top() + 5.0,
                                  fm.horizontalAdvance(idx_text) + 8.0, 14.0);
                p.setRenderHint(QPainter::Antialiasing, false);
                p.setBrush(t.surface_highest);
                p.setPen(Qt::NoPen);
                p.drawRoundedRect(chip, 7.0, 7.0);
                p.setPen(t.ink);
                p.drawText(chip, Qt::AlignCenter, idx_text);
                p.setRenderHint(QPainter::Antialiasing, true);

                const QString name = QString::fromStdString(
                    clip.name.empty() ? "Clip" : clip.name);
                p.setPen(with_alpha(t.ink, 170));
                QString label = cell.width() > 150.0
                    ? name + QStringLiteral("  ") + QString::number(clip.src_in)
                    : name;
                label = fm.elidedText(label, Qt::ElideRight, int(cell.width() - chip.width() - 8.0));
                p.drawText(QPointF(cell.left() + chip.width() + 6.0,
                                   cell.center().y() + fm.ascent() / 2.0 - 1.0),
                           label);
            }
            ++idx;
        }
    }

    // Flat audio band underneath: dim bars sized by clip length.
    const canvas::core::Track* audio_track = nullptr;
    for (const auto& track : sequence_->audio_tracks) {
        if (!track.clips.empty()) audio_track = &track;
    }
    p.setBrush(t.surface_low);
    p.setPen(QPen(t.border_soft, 1.0));
    p.drawRoundedRect(audio_row, 4.0, 4.0);
    if (audio_track) {
        for (const auto& clip : audio_track->clips) {
            const double x0 = audio_row.left() + clip.tl_in * px_per_frame;
            const double x1 = audio_row.left() + clip.tl_out * px_per_frame;
            p.setPen(Qt::NoPen);
            p.setBrush(with_alpha(t.accent, clip.enabled ? 120 : 40));
            p.drawRoundedRect(QRectF(std::max(x0, audio_row.left()),
                                     audio_row.top() + 3.0,
                                     std::max(2.0, x1 - x0),
                                     audio_row.height() - 6.0),
                              2.0, 2.0);
        }
    }

    // Playhead.
    const double phx = video_row.left() + playhead_ * px_per_frame;
    if (phx >= video_row.left()) {
        p.setPen(QPen(t.playhead, 1.4));
        p.drawLine(QPointF(phx, video_row.top() - 2.0), QPointF(phx, audio_row.bottom() + 2.0));
        p.setBrush(t.playhead);
        p.setPen(Qt::NoPen);
        p.drawPolygon(QPolygonF{QPointF(phx - 4.0, video_row.top() - 2.0),
                                QPointF(phx + 4.0, video_row.top() - 2.0),
                                QPointF(phx, video_row.top() - 6.0)});
    }
}

void MiniTimelineStrip::mousePressEvent(QMouseEvent* event) {
    if (event->button() != Qt::LeftButton || !sequence_) return;
    const QRectF video_row(8.0, 6.0, width() - 16.0, 24.0);
    if (!video_row.contains(event->position())) return;
    const int64_t total = total_frames();
    const double px_per_frame = video_row.width() / double(total);
    const int64_t frame = int64_t((event->position().x() - video_row.left()) / px_per_frame);

    for (const auto& track : sequence_->video_tracks) {
        for (const auto& clip : track.clips) {
            if (frame >= clip.tl_in && frame < clip.tl_out) {
                emit clip_activated(clip.id, clip.tl_in);
                return;
            }
        }
    }
}

}  // namespace canvas::gui