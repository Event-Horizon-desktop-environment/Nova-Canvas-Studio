#include "features/color/scopes/common/scope_common.hpp"

#include <QFont>
#include <QImage>
#include <QPainter>
#include <QPen>

#include <algorithm>
#include <cmath>
#include <utility>

#include "UX/theme.hpp"
#include "canvas/core/gpu/colorspace.hpp"

namespace canvas::gui {

namespace {
constexpr int kPlotMargin = 6;
constexpr int kGridDivisions = 8;  // 8 even divisions, labels 1023..0 (parade spec §3)
}  // namespace

// ── ColumnHistogram ─────────────────────────────────────────────────────────

void ColumnHistogram::clear() {
    hist_.fill(0);
    hist_luma_.fill(0);
}

void ColumnHistogram::accumulate(const canvas::core::VideoFrame& rgba) {
    const int w = rgba.width;
    const int h = rgba.height;
    if (w <= 0 || h <= 0) return;
    const std::size_t stride = rgba.stride != 0 ? rgba.stride : std::size_t(w) * 4;

    // Stride-sample so the total pixel count lands at/under kScopeTargetSamples.
    const double need = std::sqrt(double(w) * double(h) / double(kScopeTargetSamples));
    const int sy = std::max(1, static_cast<int>(need));
    const int sx = std::max(1, static_cast<int>(need));

    const uint8_t* base = rgba.rgba.data();
    for (int row = 0; row < h; row += sy) {
        const uint8_t* p = base + std::size_t(row) * stride;
        for (int col = 0; col < w; col += sx) {
            const std::size_t o = std::size_t(col) * 4;
            const std::uint32_t lvl_r = p[o + 0];
            const std::uint32_t lvl_g = p[o + 1];
            const std::uint32_t lvl_b = p[o + 2];
            const std::size_t xoff = std::size_t((col * kScopeCols) / w) * kScopeLevels;
            hist_[0 * kScopeCellCount + xoff + lvl_r]++;
            hist_[1 * kScopeCellCount + xoff + lvl_g]++;
            hist_[2 * kScopeCellCount + xoff + lvl_b]++;
            // Rec.601 luma matching the BT.601 decode pipeline (wave spec §1:
            // key the matrix off your pipeline's color space).
            hist_luma_[xoff + (299u * lvl_r + 587u * lvl_g + 114u * lvl_b) / 1000u]++;
        }
    }
}

void ColumnHistogram::accumulate(const canvas::core::Nv12Frame& nv12) {
    const int w = nv12.width;
    const int h = nv12.height;
    if (w <= 0 || h <= 0) return;

    const double need = std::sqrt(double(w) * double(h) / double(kScopeTargetSamples));
    const int sy = std::max(1, static_cast<int>(need));
    const int sx = std::max(1, static_cast<int>(need));

    const uint8_t* y = nv12.y.data();
    const uint8_t* uv = nv12.uv.data();
    for (int row = 0; row < h; row += sy) {
        const uint8_t* yrow = y + std::size_t(row) * nv12.y_pitch;
        for (int col = 0; col < w; col += sx) {
            const std::size_t uvo = std::size_t((row / 2) * nv12.uv_pitch) + std::size_t((col / 2) * 2);
            const auto rgb = canvas::core::gpu::yuv_to_rgb(
                yrow[col], uv[uvo + 0], uv[uvo + 1]);
            const std::size_t xoff = std::size_t((col * kScopeCols) / w) * kScopeLevels;
            hist_[0 * kScopeCellCount + xoff + rgb.r]++;
            hist_[1 * kScopeCellCount + xoff + rgb.g]++;
            hist_[2 * kScopeCellCount + xoff + rgb.b]++;
            // NV12's Y plane IS the luma trace — no matrix needed.
            hist_luma_[xoff + yrow[col]]++;
        }
    }
}

// ── ScopePane ───────────────────────────────────────────────────────────────

void ScopePane::update_frame(canvas::core::RenderFramePtr frame) {
    if (cached_ == frame) {
        update();
        return;
    }
    cached_ = std::move(frame);
    recompute_render();
    update();
}

void ScopePane::paintEvent(QPaintEvent* event) {
    Q_UNUSED(event);
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const QRectF plot = paint_scope_chrome(p, *this);
    p.setClipRect(plot);
    paint_body(p, plot);
    p.setClipping(false);
}

// ── Shared density/bar rendering ────────────────────────────────────────────

double density_alpha(std::uint32_t count, double log_max) {
    const double x = std::log1p(static_cast<double>(count)) / log_max;
    return std::min(1.0, std::pow(x, 0.6));
}

QImage log_density_plane(const std::uint32_t* hist, int cols, int levels,
                         const QColor& color) {
    QImage img(cols, levels, QImage::Format_ARGB32_Premultiplied);
    img.fill(QColor(0, 0, 0, 0));

    std::uint32_t max_count = 1;
    const std::size_t cells = std::size_t(cols) * std::size_t(levels);
    for (std::size_t i = 0; i < cells; ++i) {
        max_count = std::max(max_count, hist[i]);
    }
    const double log_max = std::log1p(static_cast<double>(max_count));

    const int r = color.red();
    const int g = color.green();
    const int b = color.blue();
    for (int col = 0; col < cols; ++col) {
        const std::size_t base = std::size_t(col) * std::size_t(levels);
        for (int level = 0; level < levels; ++level) {
            const std::uint32_t n = hist[base + std::size_t(level)];
            if (n == 0) continue;
            // Log-scaled density (parade spec §2 step 4): a single stray bright
            // pixel is a faint dot while a solid region glows.
            const double a = density_alpha(n, log_max);
            const std::uint32_t qa = static_cast<std::uint32_t>(a * 255.0 + 0.5);
            std::uint32_t* dst_row = reinterpret_cast<std::uint32_t*>(
                img.scanLine(levels - 1 - level));
            dst_row[col] = (qa << 24) | ((std::uint32_t(r) * qa / 255) << 16) |
                           ((std::uint32_t(g) * qa / 255) << 8) |
                           ((std::uint32_t(b) * qa / 255));
        }
    }
    return img;
}

QImage log_bar_plane(const std::uint32_t* counts, int levels, const QColor& color) {
    QImage img(levels, levels, QImage::Format_ARGB32_Premultiplied);
    img.fill(QColor(0, 0, 0, 0));

    std::uint32_t max_count = 1;
    for (int l = 0; l < levels; ++l) max_count = std::max(max_count, counts[l]);
    const double log_max = std::log1p(static_cast<double>(max_count));

    // Resolve-style histogram bars: a 1px column per level with a 1px gap
    // between neighbours (odd levels carve the gap), a 2px opaque
    // channel-colored cap on top (adjacent bars fuse caps into a continuous
    // contour line), and a body that fades from the cap down to the floor.
    // The gaps + fade are what keep a full-tonal-range frame reading as
    // discrete bars instead of one fused slab.
    constexpr int kBarCapPx = 2;
    constexpr unsigned kBodyAlphaBase = 24;  // at the bar floor
    constexpr unsigned kBodyAlphaCap = 140;  // right under the cap
    const int r = color.red();
    const int g = color.green();
    const int b = color.blue();
    for (int level = 0; level < levels; ++level) {
        if (counts[level] == 0) continue;
        if ((level & 1) != 0) continue;  // leave the inter-bar gap column
        const double v = std::log1p(static_cast<double>(counts[level])) / log_max;
        const int bar = std::max(1, static_cast<int>(std::floor(v * (levels - 2))));
        for (int rr = 0; rr <= bar; ++rr) {
            std::uint32_t* dst_row =
                reinterpret_cast<std::uint32_t*>(img.scanLine(levels - 1 - rr));
            if (rr >= bar - (kBarCapPx - 1)) {
                // 2px cap (opaque, straight channel color).
                dst_row[level] = (255u << 24) | (std::uint32_t(r) << 16) |
                                 (std::uint32_t(g) << 8) | std::uint32_t(b);
            } else {
                // Body (premultiplied translucent fill), fading floor→cap.
                const int body_rows = bar - (kBarCapPx - 1);
                const double t = body_rows > 1 ? double(rr) / double(body_rows - 1) : 1.0;
                const unsigned qa = kBodyAlphaBase +
                    std::uint32_t((kBodyAlphaCap - kBodyAlphaBase) * t + 0.5);
                dst_row[level] = (qa << 24) | ((std::uint32_t(r) * qa / 255) << 16) |
                                 ((std::uint32_t(g) * qa / 255) << 8) |
                                 ((std::uint32_t(b) * qa / 255));
            }
        }
    }
    return img;
}

// ── Chrome + gridlines ──────────────────────────────────────────────────────

QRectF paint_scope_chrome(QPainter& p, const QWidget& host) {
    const ThemeTokens& t = tokens();

    const QRectF r(kPlotMargin, kPlotMargin, host.width() - 2.0 * kPlotMargin,
                   host.height() - 2.0 * kPlotMargin);
    p.setBrush(t.surface_low);
    p.setPen(QPen(t.border, 1.0));
    p.drawRoundedRect(r, 8.0, 8.0);

    const QRectF plot = r.adjusted(2.0, 2.0, -2.0, -2.0);
    p.fillRect(plot, QColor(3, 4, 7));
    return plot;
}

void paint_column_grid(QPainter& p, const QRectF& plot, bool channel_split) {
    const ThemeTokens& t = tokens();

    if (channel_split) {
        // Hairline vertical separators between the three parade thirds
        // (parade spec §3: touching thirds, hairline gap).
        p.setPen(QPen(with_alpha(t.ink, 36), 1.0));
        for (int g = 1; g < 3; ++g) {
            const double x = plot.left() + g * plot.width() / 3.0;
            p.drawLine(QPointF(x, plot.top()), QPointF(x, plot.bottom()));
        }
        // Frame-position labels along the bottom of each third (parade x-axis
        // is IMAGE position, not brightness — the 0..1023 levels stay on the
        // left edge as the per-window vertical scale).
        p.setFont(QFont(p.font().family(), 6));
        p.setPen(with_alpha(t.ink, 120));
        static const char* kW_kPos[3] = {"0%", "50%", "100%"};
        for (int g = 0; g < 3; ++g) {
            const double x = plot.left() + g * plot.width() / 3.0 + plot.width() / 6.0;
            p.drawText(QRectF(x - 14.0, plot.bottom() - 3.0, 28.0, 9.0),
                       Qt::AlignCenter, QString::fromUtf8(kW_kPos[g]));
        }
    }

    const QColor line = with_alpha(t.ink, 40);
    const QColor label = with_alpha(t.ink, 150);
    p.setPen(QPen(line, 1.0));
    const auto f = p.font();
    p.setFont(QFont(f.family(), 7));
    for (int d = 0; d <= kGridDivisions; ++d) {
        const int level = d * (kScopeLevels / kGridDivisions);
        const double y = plot.bottom() - (level * plot.height()) / double(kScopeLevels);
        p.drawLine(QPointF(plot.left(), y), QPointF(plot.right(), y));
        const int code = d == kGridDivisions ? 1023 : level * 4;
        p.setPen(label);
        p.drawText(QPointF(plot.left() + 3.0, y - 1.0), QString::number(code));
        p.setPen(QPen(line, 1.0));
    }
}

}  // namespace canvas::gui