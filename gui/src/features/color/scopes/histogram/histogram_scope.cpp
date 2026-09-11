#include "features/color/scopes/histogram/histogram_scope.hpp"

#include <QFont>
#include <QPainter>
#include <QPen>

#include <cstdint>

#include "UX/theme.hpp"
#include "canvas/core/gpu/colorspace.hpp"
#include "canvas/core/util/color_log.hpp"

namespace canvas::gui {

namespace {
// Vertical brightness guides (0/25/50/75/100%) + 0..255 base labels. A
// histogram's x-axis is VALUE (brightness), never image position, so it must
// not reuse the waveform/parade horizontal-luma grid with its 0..1023
// left-edge scale — that axis reads as "luminance up, image position right",
// which is exactly why it looked like an RGB parade instead of a histogram.
// `stacked` (RGB display) adds the two hairline separators between the three
// channel panes and a faint R/G/B tag at the top-left of each pane.
void paint_histogram_grid(QPainter& p, const QRectF& plot, bool stacked) {
    const ThemeTokens& t = tokens();

    if (stacked) {
        p.setFont(QFont(p.font().family(), 7));
        static constexpr char kChannelLabels[3] = {'R', 'G', 'B'};
        for (int ch = 0; ch < 3; ++ch) {
            const double pane_top = plot.top() + ch * plot.height() / 3.0;
            if (ch > 0) {
                p.setPen(QPen(with_alpha(t.ink, 36), 1.0));
                p.drawLine(QPointF(plot.left(), pane_top), QPointF(plot.right(), pane_top));
            }
            p.setPen(with_alpha(t.ink, 150));
            p.drawText(QPointF(plot.left() + 4.0, pane_top + 9.0),
                       QString(1, QChar(kChannelLabels[ch])));
        }
        const QFont f = p.font();
        p.setFont(QFont(f.family(), 6));
    }

    p.setPen(QPen(with_alpha(t.ink, 40), 1.0, Qt::DashLine));
    for (int g = 1; g < 4; ++g) {
        const double x = plot.left() + g * plot.width() / 4.0;
        p.drawLine(QPointF(x, plot.top()), QPointF(x, plot.bottom()));
    }
    p.setPen(QPen(with_alpha(t.ink, 55), 1.0));
    p.drawLine(plot.bottomLeft(), plot.bottomRight());

    p.setFont(QFont(p.font().family(), 6));
    for (int g = 0; g <= 4; ++g) {
        const int level = g * (kScopeLevels / 4);  // 0, 64, 128, 192, 256
        const double x = plot.left() + g * plot.width() / 4.0;
        p.setPen(with_alpha(t.ink, 150));
        p.drawText(QRectF(x - 12.0, plot.bottom() - 10.0, 24.0, 9.0),
                   Qt::AlignCenter,
                   QString::number(level >= kScopeLevels ? kScopeLevels - 1 : level));
    }
}
}  // namespace

void HistogramScope::recompute_render() {
    hist_.clear();
    const auto& frame = cached();
    if (!frame) return;
    if (frame->a && !frame->a->rgba.empty()) {
        hist_.accumulate(*frame->a);
    } else if (frame->nv12 && !frame->nv12->y.empty()) {
        // Color archive: [scope] spec-change line exactly once per source
        // switch — histogram readings share the frame->nv12 spec (matrix +
        // probe-reconciled range) that grades must be judged against.
        const auto& nv12 = *frame->nv12;
        if (!spec_seen_ || nv12.matrix != last_spec_matrix_ || nv12.range != last_spec_range_) {
            spec_seen_ = true;
            last_spec_matrix_ = nv12.matrix;
            last_spec_range_ = nv12.range;
            CANVAS_COLOR_LOG(
                "[scope] histogram spec matrix=%s range=%s",
                canvas::core::gpu::color_matrix_name(nv12.matrix),
                canvas::core::gpu::color_range_name(nv12.range));
        }
        hist_.accumulate(*frame->nv12);
    }
    render_density();
}

void HistogramScope::render_density() {
    // Collapsed to 1D (wave spec §3): sum each level across all columns of the
    // already-computed column buffers.
    hist1d_.fill(0);
    hist1d_luma_.fill(0);
    const auto& h = hist_.channels();
    for (int ch = 0; ch < 3; ++ch) {
        const std::uint32_t* ch_hist = h.data() + std::size_t(ch) * kScopeCellCount;
        for (int x = 0; x < kScopeCols; ++x) {
            const std::size_t base = std::size_t(x) * kScopeLevels;
            for (int level = 0; level < kScopeLevels; ++level) {
                hist1d_[std::size_t(ch) * kScopeLevels + level] += ch_hist[base + level];
            }
        }
    }
    const auto& luma = hist_.luma();
    for (int x = 0; x < kScopeCols; ++x) {
        const std::size_t base = std::size_t(x) * kScopeLevels;
        for (int level = 0; level < kScopeLevels; ++level) {
            hist1d_luma_[level] += luma[base + level];
        }
    }

    content_ = QImage(kScopeLevels, kScopeLevels, QImage::Format_ARGB32_Premultiplied);
    content_.fill(QColor(3, 4, 7));

    // RGB piles the three channels into stacked panes — red top, green middle,
    // blue bottom — each auto-scaled to its own peak (Resolve-style). Panes
    // never overlap, so there is no additive white-out; Luma keeps the single
    // full-height white bar chart.
    content_ = QImage(kScopeLevels, kScopeLevels, QImage::Format_ARGB32_Premultiplied);
    content_.fill(QColor(3, 4, 7));

    const int band = kScopeLevels / 3;  // 85 rows per pane, 3×85 = 255
    QPainter p(&content_);
    if (display_ == ScopeDisplay::Luma) {
        p.drawImage(0, 0, log_bar_plane(hist1d_luma_.data(), kScopeLevels, kScopeLumaWhite));
    } else {
        for (int ch = 0; ch < 3; ++ch) {
            p.drawImage(QRect(0, ch * band, kScopeLevels, band),
                        log_bar_plane(hist1d_.data() + std::size_t(ch) * kScopeLevels,
                                      kScopeLevels, kScopeWaveRgb[ch]));
        }
    }
}

void HistogramScope::paint_body(QPainter& p, const QRectF& plot) {
    if (!content_.isNull()) {
        p.setRenderHint(QPainter::SmoothPixmapTransform, false);
        p.drawImage(plot, content_);
    }
    paint_histogram_grid(p, plot, display_ == ScopeDisplay::Rgb);
}

}  // namespace canvas::gui