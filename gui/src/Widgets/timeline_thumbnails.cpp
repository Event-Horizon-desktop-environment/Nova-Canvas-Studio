#include "Widgets/timeline_widget.hpp"

#include "Logging.hpp"

#include "features/thumbnails/thumbnail_service.hpp"

#include <QImage>
#include <QPixmap>
#include <QRectF>
#include <QGraphicsPixmapItem>
#include <QGraphicsRectItem>

#include <algorithm>
#include <cmath>

namespace canvas::gui {

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

void TimelineWidget::set_thumbnail_service(ThumbnailService* service) {
    thumbnail_service_ = service;
    if (service) {
        connect(service, &ThumbnailService::thumbnail_ready, this, &TimelineWidget::on_thumbnail_ready);
        connect(service, &ThumbnailService::waveform_ready, this, &TimelineWidget::on_waveform_ready);
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
            // The clip's src_in..src_out window as a fraction of the media, so
            // the waveform shows exactly this clip's audio. Two clips produced
            // by a blade render the same spectrum they showed before the cut.
            float lo = 0.0f, hi = 1.0f;
            const int64_t total = it->second.total_frames;
            if (total > 0) {
                const double inv = 1.0 / static_cast<double>(total);
                lo = static_cast<float>(std::clamp(static_cast<double>(item.clip->src_in) * inv, 0.0, 1.0));
                hi = static_cast<float>(std::clamp(static_cast<double>(item.clip->src_out) * inv, 0.0, 1.0));
            }
            // Always-on diagnostic coupling this request's window-law (video-frame
            // grid: src / total_frames) with the geometry the user aims the razor
            // at, so a "wrong cut" can be reconciled against the [wave] AUDIT line
            // in the generator (audio-time grid). This is the exact pair of inputs
            // the grid cross-check needs.
            const double fpp = frames_per_pixel();
            qWarning().nospace()
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
        // Decode at ~2-3x the on-screen cell width so deep-zoom cells stay sharp;
        // the min keeps narrow cells from falling below a useful decode size.
        const int target_w = std::max(24, std::min(192, static_cast<int>(cell_w * 2.5)));
        for (int c = 0; c < num_cells; ++c) {
            const int64_t src_frame = src_in + static_cast<int64_t>((c + 0.5) * dur / num_cells);
            const uint64_t id = next_thumb_id_++;
            item.cells[c].request_id = id;
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
    for (auto& item : clip_items_) {
        if (item.track_kind != canvas::core::Track::Kind::Video) continue;
        if (item.cells.empty()) continue;
        for (auto& cell : item.cells) {
            if (cell.request_id != id || !cell.item) continue;
            // Full-width pixmap: cells butt together with no seams, so the strip
            // always reads as a continuous thumbnail preview at any zoom level.
            const double cell_w = item.rect->rect().width() / item.cells.size();
            const int cell_h = std::max(1, static_cast<int>(item.rect->rect().height() - kClipLabelHeight - 4.0));
            cell.item->setPixmap(scaled_fill(image, std::max(1, static_cast<int>(std::ceil(cell_w))), cell_h));
            return;
        }
    }
}

void TimelineWidget::on_waveform_ready(uint64_t id, const QImage& image) {
    if (image.isNull()) return;
    for (auto& item : clip_items_) {
        if (item.track_kind != canvas::core::Track::Kind::Audio) continue;
        if (item.cells.empty() || item.cells[0].request_id != id || !item.cells[0].item) continue;
        const QRectF r = item.rect->rect();
        // Fill the body EXACTLY: the pixmap is generated at r.width() (request
        // width) and must be drawn at that same width so each column maps 1:1 to
        // a timeline frame column. Rescaling to width-4 (old code) compressed the
        // spectrum ~4px inward and, with the old cx+2 inset, shifted it off the
        // frame grid — the blade wrong-cut bug. Height keeps its existing inset.
        const int cw = std::max(1, static_cast<int>(r.width()));
        const int ch = std::max(1, static_cast<int>(r.height() - kClipLabelHeight - 4.0));
        item.cells[0].item->setPixmap(QPixmap::fromImage(image).scaled(
            cw, ch, Qt::IgnoreAspectRatio, Qt::SmoothTransformation));
        // Always-on placement audit: the wave pixmap must fill the clip body EXACTLY
        // (no horizontal inset, no width-4 shrink — see timeline_view for the
        // frame_at_x law). Log the cell scene position + fpp so the drawn
        // spectrum's left/right scene-x maps back to timeline frames for
        // comparison against the clip's tl window — any mismatch here is a
        // drawn-vs-audible offset the blade would inherit.
        const double cell_x = item.cells[0].item->scenePos().x();
        const double fpp = frames_per_pixel();
        const double dxl = kSceneMargin + kTrackHeaderWidth;
        const double drawn_left_frame = (cell_x - dxl) * fpp;
        const double drawn_right_frame = (cell_x + cw - dxl) * fpp;
        qWarning().nospace()
            << "[wave] PLACE id=" << id
            << " clip=" << item.clip->id
            << " cell_scene_x=" << QString::number(cell_x, 'g', 4)
            << " cw=" << cw << " ch=" << ch
            << " fpp=" << QString::number(fpp, 'g', 4)
            << " tl=[" << item.clip->tl_in << "," << item.clip->tl_out << ")"
            << " drawn_frames=[" << QString::number(drawn_left_frame, 'f', 2) << ","
            << QString::number(drawn_right_frame, 'f', 2) << "]";
        // Render the freshly-set waveform at the clip's COMMITTED volume (a
        // rebuild re-rendered the pixmap, so this is the point where the
        // spectrum re-syncs with the gain; live drag updates come from
        // set_live_clip_gain's apply_waveform_volume_scale instead).
        apply_waveform_volume_scale(item, static_cast<float>(item.clip->volume_db));
        return;
    }
}

}  // namespace canvas::gui
