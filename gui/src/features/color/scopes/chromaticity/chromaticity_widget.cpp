#include "features/color/scopes/chromaticity/chromaticity_widget.hpp"

#include <QFont>
#include <QPainter>
#include <QPainterPath>
#include <QPen>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <utility>

#include "UX/theme.hpp"
#include "canvas/core/gpu/colorspace.hpp"
#include "features/color/scopes/common/scope_common.hpp"

namespace canvas::gui {

namespace {
// CIE 1931 2° xy spectral locus, 5 nm steps 380–700 nm (65 points). REGENERATED
// 2026-09-08 from the official CIE 2° colour-matching-function data set (CIE 018/
// ISO 11664-1, x̄ ȳ z̄ per nm) by x = X/Σ, y = Y/Σ at each wavelength — this is
// exactly how the CIE defines the locus, and the rounded values match the
// published CIE 018:2019 Table 6 / CVRL table to the printed precision. The
// horseshoe is closed along the "purple line" from 700 nm straight back to 380 nm.
constexpr double kLocus[][2] = {
    {0.1741, 0.0050}, {0.1740, 0.0050}, {0.1738, 0.0049}, {0.1736, 0.0049},
    {0.1733, 0.0048}, {0.1730, 0.0048}, {0.1726, 0.0048}, {0.1721, 0.0048},
    {0.1714, 0.0051}, {0.1703, 0.0058}, {0.1689, 0.0069}, {0.1669, 0.0086},
    {0.1644, 0.0109}, {0.1611, 0.0138}, {0.1566, 0.0177}, {0.1510, 0.0227},
    {0.1440, 0.0297}, {0.1355, 0.0399}, {0.1241, 0.0578}, {0.1096, 0.0868},
    {0.0913, 0.1327}, {0.0687, 0.2007}, {0.0454, 0.2950}, {0.0235, 0.4127},
    {0.0082, 0.5384}, {0.0039, 0.6548}, {0.0139, 0.7502}, {0.0389, 0.8120},
    {0.0743, 0.8338}, {0.1142, 0.8262}, {0.1547, 0.8059}, {0.1929, 0.7816},
    {0.2296, 0.7543}, {0.2658, 0.7243}, {0.3016, 0.6923}, {0.3374, 0.6588},
    {0.3731, 0.6245}, {0.4087, 0.5896}, {0.4441, 0.5547}, {0.4788, 0.5202},
    {0.5125, 0.4866}, {0.5448, 0.4544}, {0.5752, 0.4242}, {0.6029, 0.3965},
    {0.6270, 0.3725}, {0.6482, 0.3514}, {0.6658, 0.3340}, {0.6801, 0.3197},
    {0.6915, 0.3083}, {0.7006, 0.2993}, {0.7079, 0.2920}, {0.7140, 0.2859},
    {0.7190, 0.2809}, {0.7230, 0.2769}, {0.7260, 0.2740}, {0.7283, 0.2717},
    {0.7300, 0.2700}, {0.7311, 0.2689}, {0.7320, 0.2680}, {0.7327, 0.2673},
    {0.7334, 0.2666}, {0.7340, 0.2660}, {0.7344, 0.2656}, {0.7346, 0.2654},
    {0.7347, 0.2653},
};

// Labelled gamut triangles (spec §4 table: space name → the three primaries'
// xy + white point). Easy to extend with ACES AP0/AP1 or DaVinci Wide Gamut.
struct Gamut {
    const char* name;
    double r[2];
    double g[2];
    double b[2];
    double white[2];
};
constexpr Gamut kGamuts[3] = {
    {"Rec.709", {0.640, 0.330}, {0.300, 0.600}, {0.150, 0.060}, {0.3127, 0.3290}},
    {"DCI-P3", {0.680, 0.320}, {0.265, 0.690}, {0.150, 0.060}, {0.3127, 0.3290}},
    {"Rec.2020", {0.708, 0.292}, {0.170, 0.797}, {0.131, 0.046}, {0.3127, 0.3290}},
};

// Planckian (blackbody) chromaticities 1000–20000 K (19 points). REGENERATED
// 2026-09-08 by integrating Planck's law B(λ,T) with c₂ = 1.4388e-2 m·K against
// the official CIE 2° CMFs (360–830 nm) — same method the CIE published tables
// were built with — then normalizing to xy. Values match the classic blackbody
// locus tables (Lindbloom / Wikipedia) to ±0.0002. D65 is NOT on this curve (it
// is a daylight illuminant, slightly above the locus near 6500 K) and is drawn
// separately as the white-point dot.
constexpr double kPlanck[][2] = {
    {0.6528, 0.3445}, {0.5857, 0.3931}, {0.5267, 0.4133}, {0.4770, 0.4137},
    {0.4369, 0.4041}, {0.4053, 0.3907}, {0.3804, 0.3767}, {0.3608, 0.3635},
    {0.3451, 0.3516}, {0.3324, 0.3410}, {0.3221, 0.3318}, {0.3135, 0.3236},
    {0.3064, 0.3165}, {0.3003, 0.3103}, {0.2952, 0.3048}, {0.2869, 0.2956},
    {0.2806, 0.2883}, {0.2637, 0.2673}, {0.2565, 0.2576},
};

// Rec.709 (D65) → XYZ matrix (spec §4 step 1); input R'G'B' 0..1. Verified
// against ITU-R BT.709-6 primary set (0.4124/0.3576/0.1805, 0.2126/0.7152/0.0722,
// 0.0193/0.1192/0.9505). The two output values are set to NaN on a degenerate
// (all-black) input so the accumulation can skip them.
inline void rgb_to_xy(double r, double g, double b, double& x, double& y) {
    const double X = 0.4124 * r + 0.3576 * g + 0.1805 * b;
    const double Y = 0.2126 * r + 0.7152 * g + 0.0722 * b;
    const double Z = 0.0193 * r + 0.1192 * g + 0.9505 * b;
    const double s = X + Y + Z;
    x = X / s;
    y = Y / s;
}
}  // namespace

ChromaticityWidget::ChromaticityWidget(QWidget* parent) : QWidget(parent) {
    setMinimumSize(240, 160);
}

void ChromaticityWidget::set_frame(canvas::core::RenderFramePtr frame) {
    if (frame_ == frame) return;
    frame_ = std::move(frame);
    dirty_ = true;
    update();
}

void ChromaticityWidget::refresh() {
    dirty_ = true;
    update();
}

void ChromaticityWidget::paintEvent(QPaintEvent* event) {
    Q_UNUSED(event);
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const ThemeTokens& t = tokens();

    const QRectF plot = paint_scope_chrome(p, *this);

    if (dirty_) {
        recompute();
        dirty_ = false;
    }

    // Square-projected chart of the xy domain, centered in the plot.
    const double scale = std::min(plot.height() / (kYmax - kYmin), plot.width() / (kXmax - kXmin));
    const double chart_w = (kXmax - kXmin) * scale;
    const double chart_h = (kYmax - kYmin) * scale;
    const QRectF chart(plot.center().x() - chart_w / 2.0, plot.center().y() - chart_h / 2.0,
                       chart_w, chart_h);
    auto to_px = [&](double x, double y) {
        return QPointF(chart.left() + (x - kXmin) * scale, chart.bottom() - (y - kYmin) * scale);
    };

    p.setClipRect(chart);

    // The frame's pixel chromaticities, drawn beneath the overlays it must be
    // compared against (out-of-gamut points poke visibly outside their triangle).
    if (!scatter_.isNull()) {
        p.setRenderHint(QPainter::SmoothPixmapTransform, false);
        p.drawImage(chart, scatter_);
        p.setRenderHint(QPainter::SmoothPixmapTransform, true);
    }

    // Spectral locus, closed along the purple line (380 nm ↔ 700 nm).
    QPainterPath locus;
    const int n_locus = int(sizeof(kLocus) / sizeof(kLocus[0]));
    for (int i = 0; i < n_locus; ++i) {
        if (i == 0) {
            locus.moveTo(to_px(kLocus[i][0], kLocus[i][1]));
        } else {
            locus.lineTo(to_px(kLocus[i][0], kLocus[i][1]));
        }
    }
    locus.closeSubpath();
    p.setPen(QPen(with_alpha(t.ink, 190), 1.2));
    p.setBrush(Qt::NoBrush);
    p.drawPath(locus);

    // Gamut triangles for the delivery spaces.
    p.setFont(QFont(font().family(), 6));
    const QColor gamut_colors[3] = {QColor(0xE8, 0xE8, 0xE6),
                                    QColor(0x4C, 0xFF, 0x4C),
                                    QColor(0x55, 0x8A, 0xFF)};
    for (int gi = 0; gi < 3; ++gi) {
        const Gamut& gm = kGamuts[gi];
        QPainterPath tri;
        tri.moveTo(to_px(gm.r[0], gm.r[1]));
        tri.lineTo(to_px(gm.g[0], gm.g[1]));
        tri.lineTo(to_px(gm.b[0], gm.b[1]));
        tri.closeSubpath();
        p.setPen(QPen(with_alpha(gamut_colors[gi], 140), 1.0));
        p.drawPath(tri);
        const QPointF label = to_px(gm.r[0], gm.r[1]) + QPointF(3.0, -2.0);
        p.setPen(with_alpha(gamut_colors[gi], 180));
        p.drawText(label, QString::fromLatin1(gm.name));
    }

    // Planckian locus (blackbody curve, warm → cool) + reference white dot.
    QPainterPath planck;
    const int n_planck = int(sizeof(kPlanck) / sizeof(kPlanck[0]));
    for (int i = 0; i < n_planck; ++i) {
        if (i == 0) {
            planck.moveTo(to_px(kPlanck[i][0], kPlanck[i][1]));
        } else {
            planck.lineTo(to_px(kPlanck[i][0], kPlanck[i][1]));
        }
    }
    p.setPen(QPen(QColor(0xC9, 0x86, 0x3A), 1.0));
    p.drawPath(planck);

    p.setBrush(with_alpha(t.ink, 200));
    p.setPen(Qt::NoPen);
    p.drawEllipse(to_px(0.3127, 0.3290), 2.5, 2.5);

    p.setClipping(false);
}

void ChromaticityWidget::recompute() {
    grid_.fill(0);
    scatter_ = QImage();
    if (!frame_) return;
    // Stride-sample like the other scopes (spec §4 performance note: the 3×3
    // matrix is the most expensive conversion per pixel, so a bounded sample
    // count is essential).
    if (frame_->a && !frame_->a->rgba.empty()) {
        accumulate(*frame_->a);
    } else if (frame_->nv12 && !frame_->nv12->y.empty()) {
        accumulate(*frame_->nv12);
    }
    render_density();
}

void ChromaticityWidget::accumulate(const canvas::core::VideoFrame& rgba) {
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
            double x = 0.0;
            double y = 0.0;
            rgb_to_xy(p[o + 0] / 255.0, p[o + 1] / 255.0, p[o + 2] / 255.0, x, y);
            if (!(x >= kXmin) || !(y >= kYmin) || x > kXmax || y > kYmax) continue;
            const int gx = int((x - kXmin) / (kXmax - kXmin) * kScatterGrid);
            const int gy = int((y - kYmin) / (kYmax - kYmin) * kScatterGrid);
            grid_[std::size_t(gy) * kScatterGrid + gx]++;
        }
    }
}

void ChromaticityWidget::accumulate(const canvas::core::Nv12Frame& nv12) {
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
            const auto rgb = canvas::core::gpu::yuv_to_rgb(yrow[col], uv[uvo + 0], uv[uvo + 1]);
            double x = 0.0;
            double y = 0.0;
            rgb_to_xy(rgb.r / 255.0, rgb.g / 255.0, rgb.b / 255.0, x, y);
            if (!(x >= kXmin) || !(y >= kYmin) || x > kXmax || y > kYmax) continue;
            const int gx = int((x - kXmin) / (kXmax - kXmin) * kScatterGrid);
            const int gy = int((y - kYmin) / (kYmax - kYmin) * kScatterGrid);
            grid_[std::size_t(gy) * kScatterGrid + gx]++;
        }
    }
}

void ChromaticityWidget::render_density() {
    scatter_ = QImage(kScatterGrid, kScatterGrid, QImage::Format_ARGB32_Premultiplied);
    scatter_.fill(QColor(0, 0, 0, 0));

    std::uint32_t max_count = 1;
    for (std::uint32_t n : grid_) max_count = std::max(max_count, n);
    const double log_max = std::log1p(static_cast<double>(max_count));
    const QColor dim(0x4C, 0xFF, 0x4C);  // long-wavelength-green trace, low-key

    for (int gy = 0; gy < kScatterGrid; ++gy) {
        std::uint32_t* dst_row = reinterpret_cast<std::uint32_t*>(
            scatter_.scanLine(kScatterGrid - 1 - gy));
        for (int gx = 0; gx < kScatterGrid; ++gx) {
            const std::uint32_t n = grid_[std::size_t(gy) * kScatterGrid + gx];
            if (n == 0) continue;
            const std::uint32_t qa = static_cast<std::uint32_t>(
                std::min(1.0, std::log1p(double(n)) / log_max) * 255.0 + 0.5);
            const std::uint32_t a = std::min(std::uint32_t(60) + qa / 2, std::uint32_t(200));
            dst_row[gx] = (a << 24) | ((dim.red() * a / 255) << 16) |
                          ((dim.green() * a / 255) << 8) | ((dim.blue() * a / 255));
        }
    }
}

}  // namespace canvas::gui