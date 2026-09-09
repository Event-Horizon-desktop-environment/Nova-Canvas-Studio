#include "features/color/scopes/waveform/waveform_scope.hpp"

#include <QPainter>

#include <cstdint>

namespace canvas::gui {

void WaveformScope::recompute_render() {
    hist_.clear();
    const auto& frame = cached();
    if (!frame) return;
    if (frame->a && !frame->a->rgba.empty()) {
        hist_.accumulate(*frame->a);
    } else if (frame->nv12 && !frame->nv12->y.empty()) {
        hist_.accumulate(*frame->nv12);
    }
    render_density();
}

void WaveformScope::render_density() {
    // One plot, not three thirds (wave spec §1: RGB reuses the exact parade
    // buffers — switching Waveform/Parade never re-reads the frame).
    content_ = QImage(kScopeCols, kScopeLevels, QImage::Format_ARGB32_Premultiplied);
    content_.fill(QColor(3, 4, 7));

    QPainter p(&content_);
    p.setCompositionMode(QPainter::CompositionMode_Plus);
    if (display_ == ScopeDisplay::Luma || display_ == ScopeDisplay::Yrgb) {
        p.drawImage(0, 0, log_density_plane(
            hist_.luma().data(), kScopeCols, kScopeLevels, kScopeLumaWhite));
    }
    if (display_ == ScopeDisplay::Rgb || display_ == ScopeDisplay::Yrgb) {
        for (int ch = 0; ch < 3; ++ch) {
            p.drawImage(0, 0, log_density_plane(
                hist_.channels().data() + std::size_t(ch) * kScopeCellCount,
                kScopeCols, kScopeLevels, kScopeWaveRgb[ch]));
        }
    }
    p.setCompositionMode(QPainter::CompositionMode_SourceOver);
}

void WaveformScope::paint_body(QPainter& p, const QRectF& plot) {
    if (!content_.isNull()) {
        p.setRenderHint(QPainter::SmoothPixmapTransform, false);
        p.drawImage(plot, content_);
    }
    paint_column_grid(p, plot, /*channel_split=*/false);
}

}  // namespace canvas::gui