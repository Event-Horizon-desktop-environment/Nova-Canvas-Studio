#include "features/color/scopes/vectorscope/vectorscope_scope.hpp"

#include <QFont>
#include <QImage>
#include <QPainter>
#include <QPainterPath>
#include <QPen>

#include <algorithm>
#include <cmath>

#include "UX/theme.hpp"
#include "canvas/core/gpu/colorspace.hpp"
#include "canvas/core/util/color_log.hpp"
#include "canvas/core/util/log.hpp"

namespace canvas::gui {

namespace {
// Rec.601 steps of the Y'CbCr matrix (wave spec §2: values range roughly
// -0.5..0.5 for in-gamut colors). Matches the decode pipeline's BT.601.
constexpr double kCbR = -0.168736;
constexpr double kCbG = -0.331264;
constexpr double kCbB = 0.500000;
constexpr double kCrR = 0.500000;
constexpr double kCrG = -0.418688;
constexpr double kCrB = -0.081312;

// Six color-bar targets drawn at the 75% color-bars positions — the broadcast-
// legal amplitude used by SMPTE bars and every real vectorscope graticule
// (Tektronix, UltraScope, DaVinci Resolve, Wolfram "SafeTargets"). Computed,
// never hardcoded angles: the full-saturation R/Mg/B/Cy/G/Yl run through the
// exact Rec.601 matrix (wave spec §2 step 4), then scaled by 0.75.
struct VecTarget {
    double cb;
    double cr;
    const char* label;
};
constexpr VecTarget kTargets75[6] = {
    {-0.127, 0.375, "R"},   {0.248, 0.314, "Mg"},  {0.375, -0.061, "B"},
    {0.127, -0.375, "Cy"},  {-0.248, -0.314, "G"}, {-0.375, 0.061, "Yl"},
};

// Angles in the scope's polar basis (math convention: CCW from +Cb, +Cr up,
// so +U points right and +V up — matching the kTargets75 box orientation).
// The NTSC I axis is the skin-tone line reference at 123° (a few degrees past
// R toward Yl — the red-to-orange band real skin tones land in); Q lags 90°.
constexpr double kSkinAngle = 123.0 * M_PI / 180.0;
constexpr double kQAngle = kSkinAngle - M_PI / 2.0;

// Compass-rose granularity (every 30° gets a numeral, matching the Tektronix
// graticule's major ticks).
constexpr int kCompassStep = 30;
}  // namespace

void VectorscopeScope::recompute_render() {
    scatter_.fill(ScatterCell{});
    const auto& frame = cached();
    if (!frame) return;
    if (frame->a && !frame->a->rgba.empty()) {
        accumulate(*frame->a);
    } else if (frame->nv12 && !frame->nv12->y.empty()) {
        accumulate(*frame->nv12);
    }
    render_density();
}

void VectorscopeScope::accumulate(const canvas::core::VideoFrame& rgba) {
    const int w = rgba.width;
    const int h = rgba.height;
    if (w <= 0 || h <= 0) return;
    const std::size_t stride = rgba.stride != 0 ? rgba.stride : std::size_t(w) * 4;

    const double need = std::sqrt(double(w) * double(h) / double(kScopeTargetSamples));
    const int sy = std::max(1, static_cast<int>(need));
    const int sx = std::max(1, static_cast<int>(need));

    const uint8_t* base = rgba.rgba.data();
    for (int row = 0; row < h; row += sy) {
        const uint8_t* p = base + std::size_t(row) * stride;
        for (int col = 0; col < w; col += sx) {
            const std::size_t o = std::size_t(col) * 4;
            const double nr = p[o + 0] / 255.0;
            const double ng = p[o + 1] / 255.0;
            const double nb = p[o + 2] / 255.0;
            const double cb = kCbR * nr + kCbG * ng + kCbB * nb;
            const double cr = kCrR * nr + kCrG * ng + kCrB * nb;
            const int cbi = std::clamp(int((cb + 0.5) * kScopeVec), 0, kScopeVec - 1);
            const int cri = std::clamp(int((cr + 0.5) * kScopeVec), 0, kScopeVec - 1);
            ScatterCell& cell = scatter_[std::size_t(cri) * kScopeVec + cbi];
            cell.count++;
            cell.r_sum += p[o + 0];
            cell.g_sum += p[o + 1];
            cell.b_sum += p[o + 2];
        }
    }
}

void VectorscopeScope::accumulate(const canvas::core::Nv12Frame& nv12) {
    // Color archive: log a [scope] spec-change line exactly once per source
    // switch (matrix/range only change when the media under the playhead does),
    // so wheel interactions can be correlated with the spec the vectorscope was
    // analysing. Scopes consume the same frame->nv12 spec as the viewer shader.
    if (!spec_seen_ || nv12.matrix != last_spec_matrix_ || nv12.range != last_spec_range_) {
        spec_seen_ = true;
        last_spec_matrix_ = nv12.matrix;
        last_spec_range_ = nv12.range;
        CANVAS_COLOR_LOG(
            "[scope] vectorscope spec matrix=%s range=%s",
            canvas::core::gpu::color_matrix_name(nv12.matrix),
            canvas::core::gpu::color_range_name(nv12.range));
    }
    // Always-on (once): scope NV12->RGB uses the frame's resolved per-file spec
    // (matrix + probe-reconciled range) — matching the viewer shader on this
    // frame, NOT a compile-time BT.709 limited assumption.
    static bool yuv2rgb_logged_ = false;
    if (!yuv2rgb_logged_) {
        yuv2rgb_logged_ = true;
        ::canvas::core::log::log_warning(
            "[vectorscope] yuv_to_rgb uses per-frame Nv12Frame spec matrix=%s range=%s",
            canvas::core::gpu::color_matrix_name(nv12.matrix),
            canvas::core::gpu::color_range_name(nv12.range));
    }
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
            const auto rgb = canvas::core::gpu::yuv_to_rgb(yrow[col], uv[uvo + 0], uv[uvo + 1],
                                                           nv12.range, nv12.matrix);
            const double nr = rgb.r / 255.0;
            const double ng = rgb.g / 255.0;
            const double nb = rgb.b / 255.0;
            const double cb = kCbR * nr + kCbG * ng + kCbB * nb;
            const double cr = kCrR * nr + kCrG * ng + kCrB * nb;
            const int cbi = std::clamp(int((cb + 0.5) * kScopeVec), 0, kScopeVec - 1);
            const int cri = std::clamp(int((cr + 0.5) * kScopeVec), 0, kScopeVec - 1);
            ScatterCell& cell = scatter_[std::size_t(cri) * kScopeVec + cbi];
            cell.count++;
            cell.r_sum += rgb.r;
            cell.g_sum += rgb.g;
            cell.b_sum += rgb.b;
        }
    }
}

void VectorscopeScope::render_density() {
    // Log-normalized density (wave spec §2 step 4: logarithmic exposure curve),
    // shaded per TraceMode. The gain slider is a display-time multiplier on the
    // bucket opacity, never a change to the accumulated math.
    content_ = QImage(kScopeVec, kScopeVec, QImage::Format_ARGB32_Premultiplied);
    content_.fill(QColor(0, 0, 0, 0));

    std::uint32_t max_count = 1;
    for (const ScatterCell& cell : scatter_) {
        max_count = std::max(max_count, cell.count);
    }
    const double log_max = std::log1p(static_cast<double>(max_count));
    const double gain = double(gain_percent_) / 100.0;
    // True-color core burn: buckets whose density crosses this fraction of the
    // brightest bucket overexpose to white, like a real scope's hot spot.
    constexpr double kCoreBurn = 0.80;

    const QColor phosphor = kScopeWaveRgb[1];
    const int p_r = phosphor.red();
    const int p_g = phosphor.green();
    const int p_b = phosphor.blue();

    for (int cri = 0; cri < kScopeVec; ++cri) {
        std::uint32_t* dst_row = reinterpret_cast<std::uint32_t*>(
            content_.scanLine(kScopeVec - 1 - cri));
        for (int cbi = 0; cbi < kScopeVec; ++cbi) {
            const ScatterCell& cell = scatter_[std::size_t(cri) * kScopeVec + cbi];
            if (cell.count == 0) continue;
            const double d = std::min(1.0, std::log1p(double(cell.count)) / log_max);
            const std::uint32_t qa = std::min(
                std::uint32_t(255),
                std::uint32_t(d * 255.0 * gain + 0.5));
            if (qa == 0) continue;

            int r = 255;
            int g = 255;
            int b = 255;
            switch (trace_mode_) {
                case TraceMode::Mono:
                    break;  // white
                case TraceMode::Green:
                    r = p_r;
                    g = p_g;
                    b = p_b;
                    break;
                case TraceMode::Color:
                    if (d < kCoreBurn) {  // keep dense core white (overexposed)
                        r = static_cast<int>(cell.r_sum / cell.count);
                        g = static_cast<int>(cell.g_sum / cell.count);
                        b = static_cast<int>(cell.b_sum / cell.count);
                    }
                    break;
            }
            dst_row[cbi] = (qa << 24) |
                           ((std::uint32_t(r) * qa / 255) << 16) |
                           ((std::uint32_t(g) * qa / 255) << 8) |
                           (std::uint32_t(b) * qa / 255);
        }
    }
}

void VectorscopeScope::paint_body(QPainter& p, const QRectF& plot) {
    const double side = std::min(plot.width(), plot.height()) * 0.90;
    const QPointF c = plot.center();
    const QRectF sq(c.x() - side / 2.0, c.y() - side / 2.0, side, side);

    // Density cloud, clipped to the graticule disk. 2x is a trace-gain zoom —
    // the analogue of a real scope's VARIABLE chroma gain: only the trace
    // scales, the graticule stays fixed on screen (scaling both would push
    // the ring and compass ticks off the widget, which is what the old code
    // did). The cloud always maps the full ±0.5 Cb/Cr span onto the disk.
    p.save();
    QPainterPath disk;
    disk.addEllipse(c, side / 2.0, side / 2.0);
    p.setClipPath(disk);
    p.setRenderHint(QPainter::SmoothPixmapTransform, true);
    if (zoom2x_) {
        p.translate(c);
        p.scale(2.0, 2.0);
        p.translate(-c);
    }
    p.drawImage(sq, content_);
    p.restore();

    paint_graticule(p, plot);
}

void VectorscopeScope::paint_graticule(QPainter& p, const QRectF& plot) const {
    const ThemeTokens& t = tokens();
    const double side = std::min(plot.width(), plot.height()) * 0.90;
    const QPointF c = plot.center();
    const double half = side / 2.0;

    // Cb/Cr (both in [-0.5, 0.5]) → plot pixel mapping; y grows upward.
    auto to_plot = [&](double cb, double cr) {
        return QPointF(c.x() + (cb / 0.5) * half, c.y() - (cr / 0.5) * half);
    };
    // Math-convention polar: angle CCW from +Cb (right), +Cr up. Angle 0 is
    // +U, 90° is +V, so the compass rose matches the box geometry.
    auto polar = [&](double angle, double radius) {
        return c + QPointF(std::cos(angle), std::sin(angle)) * radius;
    };
    const double rad_deg = M_PI / 180.0;

    // paint_scope_chrome leaves the painter's brush at `surface_low` for the
    // next paint call. Clear it: the graticule rings/ellipses below must be
    // outlines only — otherwise drawEllipse fills an opaque greyish-blue disc
    // over the chroma cloud (the "grey circle in the middle" bug).
    p.setBrush(Qt::NoBrush);

    p.setClipRect(QRectF(c.x() - half, c.y() - half, side, side));

    // ±U (B-Y, horizontal) / ±V (R-Y, vertical) component axes.
    p.setPen(QPen(with_alpha(t.ink, 42), 1.0));
    p.drawLine(c.x() - half, c.y(), c.x() + half, c.y());
    p.drawLine(c.x(), c.y() - half, c.x(), c.y() + half);

    // I/Q diameters (NTSC graticule signature). +I lands on the skin-tone
    // line, drawn as an amber ray past R toward Yl; the -I and Q segments are
    // a faint dotted cross so they read as axes, not content.
    p.setPen(QPen(QColor(0xC9, 0x86, 0x3A, 170), 1.0));
    p.drawLine(c, polar(kSkinAngle, half * 0.98));
    p.setPen(QPen(with_alpha(t.ink, 52), 1.0, Qt::DotLine));
    p.drawLine(c, polar(kSkinAngle + M_PI, half * 0.98));
    p.drawLine(polar(kQAngle, half * 0.98), polar(kQAngle + M_PI, half * 0.98));

    // 75% color-bar calibration ring: the six box-target centers sit exactly
    // on it, so feeding SMPTE 75% bars lands the dots in the boxes.
    p.setPen(QPen(with_alpha(t.ink, 70), 1.0, Qt::DashLine));
    p.drawEllipse(c, half * 0.75, half * 0.75);

    // Six box-target squares + labels just beyond the ring, toward the edge
    // (Tektronix layout: label between the box and the outer circle).
    p.setFont(QFont(p.font().family(), 6));
    for (const VecTarget& tg : kTargets75) {
        const QPointF pt = to_plot(tg.cb, tg.cr);
        p.setPen(QPen(with_alpha(t.ink, 185), 1.0));
        p.setBrush(Qt::NoBrush);
        p.drawRect(QRectF(pt.x() - 4.0, pt.y() - 4.0, 8.0, 8.0));
        const double ang = std::atan2(tg.cr, tg.cb);
        const QPointF lab = polar(ang, half * 0.81);
        p.setPen(with_alpha(t.ink, 168));
        p.drawText(QRectF(lab.x() - 12.0, lab.y() - 5.0, 24.0, 10.0),
                   Qt::AlignCenter, QString::fromLatin1(tg.label));
    }

    // Outer circle = full-swing Cb/Cr limit, with a 10° compass rose; major
    // ticks every 30° with numerals (Tektronix-style).
    p.setPen(QPen(with_alpha(t.ink, 115), 1.0));
    p.drawEllipse(c, half, half);
    for (int deg = 0; deg < 360; deg += 10) {
        const bool major = (deg % kCompassStep) == 0;
        const double a = deg * rad_deg;
        p.setPen(QPen(with_alpha(t.ink, major ? 165 : 85), major ? 1.2 : 1.0));
        p.drawLine(polar(a, half), polar(a, half + (major ? 7.0 : 4.0)));
    }
    p.setFont(QFont(p.font().family(), 5));
    for (int deg = 0; deg < 360; deg += kCompassStep) {
        const QPointF lp = polar(deg * rad_deg, half * 0.90);
        p.setPen(with_alpha(t.ink, 140));
        p.drawText(QRectF(lp.x() - 8.0, lp.y() - 4.5, 16.0, 9.0), Qt::AlignCenter,
                   QString::number(deg) + QChar(0x00B0));
    }

    // Cardinal tip dots where ±U / ±V cross the outer ring.
    p.setPen(Qt::NoPen);
    p.setBrush(with_alpha(t.ink, 170));
    for (double a : {0.0, M_PI / 2.0, M_PI, 3.0 * M_PI / 2.0}) {
        p.drawEllipse(polar(a, half), 1.5, 1.5);
    }
}

}  // namespace canvas::gui