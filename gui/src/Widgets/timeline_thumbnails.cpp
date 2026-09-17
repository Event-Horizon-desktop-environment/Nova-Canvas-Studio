#include "Widgets/timeline_widget.hpp"

#include "Logging.hpp"

#include "UX/theme.hpp"

#include "features/thumbnails/thumbnail_service.hpp"

#include <QImage>
#include <QPixmap>
#include <QRectF>
#include <QGraphicsPixmapItem>
#include <QGraphicsRectItem>
#include <QPainter>
#include <QPen>

#include <algorithm>
#include <cmath>

namespace canvas::gui {

QPixmap scaled_fit(const QImage& img, int w, int h) {
    if (w <= 0 || h <= 0 || img.isNull()) return QPixmap();
    const QImage scaled =
        img.scaled(w, h, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    QPixmap canvas(w, h);
    canvas.fill(Qt::transparent);
    QPainter p(&canvas);
    p.drawImage((w - scaled.width()) / 2, (h - scaled.height()) / 2, scaled);
    p.end();
    return canvas;
}

void TimelineWidget::set_thumbnail_service(ThumbnailService* service) {
    thumbnail_service_ = service;
    if (service) {
        connect(service, &ThumbnailService::thumbnail_ready, this,
                &TimelineWidget::on_thumbnail_ready, Qt::QueuedConnection);
        connect(service, &ThumbnailService::waveform_ready, this,
                &TimelineWidget::on_waveform_ready, Qt::QueuedConnection);
    }
}

void TimelineWidget::request_clip_thumbnails() {
    if (!thumbnail_service_ || media_paths_.empty() || !sequence_) {
        if (debug_enabled())
            qDebug() << "thumb: request_clip_thumbnails SKIPPED (no service/paths/sequence)";
        return;
    }
    int video_clips = 0, audio_clips = 0, total_requests = 0, skipped = 0;
    for (auto& item : clip_items_) {
        const auto it = media_paths_.find(item.clip->media);
        if (it == media_paths_.end()) { skipped++; continue; }

        if (item.track_kind == canvas::core::Track::Kind::Audio) {
            const double cw = item.rect->rect().width();
            if (cw <= 0 || item.cells.empty()) { skipped++; continue; }
            audio_clips++;
            const int clip_h = std::max(1, static_cast<int>(item.rect->rect().height() - kClipLabelHeight - 4));
            const uint64_t id = next_thumb_id_++;
            item.cells[0].request_id = id;
            float lo = 0.0f, hi = 1.0f;
            const int64_t total = it->second.total_frames;
            if (total > 0) {
                const double inv = 1.0 / static_cast<double>(total);
                lo = static_cast<float>(std::clamp(static_cast<double>(item.clip->src_in) * inv, 0.0, 1.0));
                hi = static_cast<float>(std::clamp(static_cast<double>(item.clip->src_out) * inv, 0.0, 1.0));
            }
            const double fpp = frames_per_pixel();
            qDebug().nospace()
                << "[wave] REQ id=" << id
                << " clip=" << item.clip->id
                << " src=[" << item.clip->src_in << "," << item.clip->src_out << ")"
                << " tl=[" << item.clip->tl_in << "," << item.clip->tl_out << ")"
                << " total=" << total
                << " fps=" << QString::number(it->second.fps, 'g', 4)
                << " lo=" << QString::number(lo, 'g', 6)
                << " hi=" << QString::number(hi, 'g', 6)
                << " w=" << static_cast<int>(cw)
                << " h=" << clip_h
                << " fpp=" << QString::number(fpp, 'g', 4);
            thumbnail_service_->request_waveform(id, it->second.path, static_cast<int>(cw), clip_h,
                                                 lo, hi, 1.0f, item.clip->src_in, item.clip->src_out,
                                                 item.clip->tl_in, item.clip->tl_out,
                                                 it->second.fps, total);
            total_requests++;
            continue;
        }

        const int64_t dur = std::max<int64_t>(1, item.clip->duration());
        const int64_t src_in = item.clip->src_in;
        const int num_cells = static_cast<int>(item.cells.size());
        if (num_cells == 0) { skipped++; continue; }
        video_clips++;

        const double cw = item.rect->rect().width();
        const double cell_w = cw / num_cells;
        const int64_t last_frame =
            src_in + static_cast<int64_t>((num_cells - 0.5) * dur / num_cells);
        if (debug_enabled())
            qWarning().nospace()
                << "[filmstrip] REQ clip=" << item.clip->id
                << " cells=" << num_cells
                << " cw=" << QString::number(cw, 'f', 1)
                << " cell_w=" << QString::number(cell_w, 'f', 2)
                << " fpp=" << QString::number(frames_per_pixel(), 'g', 4)
                << " dur=" << dur << " src_in=" << src_in
                << " frames=[" << src_in << "," << last_frame << "]"
                << " track=" << item.track_index;
        const int target_w = std::max(24, std::min(192, static_cast<int>(cell_w * 2.5)));
        for (int c = 0; c < num_cells; ++c) {
            const int64_t src_frame = src_in + static_cast<int64_t>((c + 0.5) * dur / num_cells);
            const uint64_t id = next_thumb_id_++;
            item.cells[c].request_id = id;
            const double cell_x = item.rect->pos().x() + c * cell_w;
            if (debug_enabled())
                qWarning().nospace()
                    << "[filmstrip] CELL id=" << id
                    << " cell=" << c << "/" << num_cells
                    << " scene_x=" << QString::number(cell_x, 'f', 2)
                    << " frame=" << src_frame;
            ThumbRequest req;
            req.id = id;
            req.path = it->second.path;
            req.frame = src_frame;
            req.target_width = target_w;
            req.max_height = static_cast<int>((track_height(item.track_index,
                                                    static_cast<int>(sequence_->video_tracks.size())) -
                                   kClipLabelHeight) * 2.0);
            thumbnail_service_->request(req);
            total_requests++;
        }
    }
    if (debug_enabled())
        qDebug() << "thumb: request_clip_thumbnails video_clips=" << video_clips
                 << "audio_clips=" << audio_clips
                 << "requests=" << total_requests << "skipped=" << skipped;
}

void TimelineWidget::on_thumbnail_ready(uint64_t id, const QImage& image) {
    if (image.isNull()) return;
    if (!is_timeline_thumb_id(id)) return;
    for (auto& item : clip_items_) {
        if (item.track_kind != canvas::core::Track::Kind::Video) continue;
        if (item.cells.empty()) continue;
        for (auto& cell : item.cells) {
            if (cell.request_id != id || !cell.item) continue;
            const double cell_w = item.rect->rect().width() / item.cells.size();
            const int cell_h = std::max(1, static_cast<int>(item.rect->rect().height() - kClipLabelHeight - 4.0));
            const int pw = std::max(1, static_cast<int>(std::ceil(cell_w)));
            const QPixmap placed = scaled_fit(image, pw, cell_h);
            if (debug_enabled())
                qWarning().nospace()
                    << "[filmstrip] PLACE id=" << id
                    << " clip=" << item.clip->id
                    << " src_dims=" << image.width() << "x" << image.height()
                    << " cell_box=" << pw << "x" << cell_h
                    << " placed_pixmap=" << placed.width() << "x" << placed.height()
                    << " cell_scene_x=" << QString::number(cell.item->pos().x(), 'f', 2)
                    << " clip_w=" << QString::number(item.rect->rect().width(), 'f', 1);
            cell.item->setPixmap(placed);
            int filled = 0;
            for (const auto& other : item.cells)
                if (other.request_id != 0 && other.item && !other.item->pixmap().isNull()) filled++;
            const int total = static_cast<int>(item.cells.size());
            if (debug_enabled())
                qWarning().nospace()
                    << "[filmstrip] STRIP clip=" << item.clip->id
                    << " filled=" << filled << "/" << total
                    << (filled >= total ? " COMPLETE" : " HOLES");
            return;
        }
    }
}

void TimelineWidget::on_waveform_ready(uint64_t id, const QImage& image) {
    if (image.isNull()) return;
    if (!is_timeline_thumb_id(id)) return;
    for (auto& item : clip_items_) {
        if (item.track_kind != canvas::core::Track::Kind::Audio) continue;
        if (item.cells.empty() || item.cells[0].request_id != id || !item.cells[0].item) continue;
        const QRectF r = item.rect->rect();
        const int cw = std::max(1, static_cast<int>(r.width()));
        const int ch = std::max(1, static_cast<int>(r.height() - kClipLabelHeight - 4.0));
        const TimelineViewOptions& vopts = eff_view_options();
        QImage shaped = image;
        if (!vopts.non_rectified_waveforms && shaped.height() > 2) {
            shaped = shaped.copy(0, shaped.height() / 2, shaped.width(),
                                 shaped.height() - shaped.height() / 2);
        }
        int th = ch;
        if (!vopts.full_waveforms) th = std::max(1, static_cast<int>(std::lround(ch * 0.62)));
        if (!vopts.scaled_waveforms) th = std::max(1, static_cast<int>(std::lround(th * 0.60)));
        QPixmap pm = QPixmap::fromImage(shaped).scaled(
            cw, th, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
        if (vopts.waveform_borders && th > 2) {
            QPixmap framed(cw, th);
            framed.fill(Qt::transparent);
            QPainter p(&framed);
            p.drawPixmap(0, 0, pm);
            p.setPen(QPen(tokens().ink_muted, 1.0));
            p.setBrush(Qt::NoBrush);
            p.drawRect(QRectF(0.5, 0.5, cw - 1.0, th - 1.0));
            p.end();
            pm = framed;
        }
        item.cells[0].item->setPixmap(pm);
        if (th < ch) {
            const double yoff = item.volume_y0 + static_cast<double>(ch - th) * 0.5;
            item.cells[0].item->setPos(item.cells[0].item->pos().x(), yoff);
        }
        const double cell_x = item.cells[0].item->scenePos().x();
        const double fpp = frames_per_pixel();
        const double dxl = kSceneMargin + kTrackHeaderWidth;
        const double drawn_left_frame = (cell_x - dxl) * fpp;
        const double drawn_right_frame = (cell_x + cw - dxl) * fpp;
        qDebug().nospace()
            << "[wave] PLACE id=" << id
            << " clip=" << item.clip->id
            << " cell_scene_x=" << QString::number(cell_x, 'g', 4)
            << " cw=" << cw << " ch=" << ch
            << " fpp=" << QString::number(fpp, 'g', 4)
            << " tl=[" << item.clip->tl_in << "," << item.clip->tl_out << ")"
            << " drawn_frames=[" << QString::number(drawn_left_frame, 'f', 2) << ","
            << QString::number(drawn_right_frame, 'f', 2) << "]";
        apply_waveform_volume_scale(item, static_cast<float>(item.clip->volume_db));
        return;
    }
}

}