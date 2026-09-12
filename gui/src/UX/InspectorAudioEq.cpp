#include "UX/InspectorAudioEq.hpp"

#include "UX/theme.hpp"

#include "canvas/core/media/equalizer.hpp"
#include "canvas/core/timeline/audio_processing.hpp"

#include <QColor>
#include <QFontMetricsF>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QTimer>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>

namespace canvas::gui {

EqGraphWidget::EqGraphWidget(QWidget* parent) : QWidget(parent) {
    bands_ = canvas::core::Clip::default_eq_bands();
    // Tall enough that plot_rect()'s dB-tick margins (22+26 px) plus the
    // node glow leave the curve and the "+24"/"-24" ticks fully visible.
    setMinimumHeight(190);
    setMouseTracking(true);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    // Debounce wheel-Q commits: a scroll gets ONE undoable edit once the
    // wheel settles (240 ms) instead of one per notch.
    wheel_timer_.setSingleShot(true);
    wheel_timer_.setInterval(240);
    connect(&wheel_timer_, &QTimer::timeout, this, [this] {
        dragging_ = false;
        if (on_commit) on_commit();
    });
    apply_theme_style(this, [] {
        const ThemeTokens& t = tokens();
        return QStringLiteral("background-color: %1; border: 1px solid %2;"
                              " border-radius: 8px;")
            .arg(css(t.surface_low), css(t.border_soft));
    });
}

void EqGraphWidget::set_bands(const Bands& bands) {
    bands_ = bands;
    if (selected_ >= 0 && selected_ < static_cast<int>(bands_.size())) {
        // Selection survives a refresh only if that band is still present
        // (it always is — six slots) — keep it so the row stays in sync.
    } else {
        selected_ = -1;
    }
    update();
    emit_selection();
}

void EqGraphWidget::set_view(View v) {
    if (view_ == v) return;
    view_ = v;
    // The two views share the same bands, but a drag's axis mental-model
    // does not survive the switch — drop any in-flight gesture and return
    // to a plain arrow so a stale press across modes can't grab a fader.
    dragging_ = false;
    drag_band_ = -1;
    hovered_ = -1;
    wheel_timer_.stop();
    setCursor(Qt::ArrowCursor);
    update();
}

void EqGraphWidget::set_selected(int index) {
    selected_ = index;
    update();
    emit_selection();
}

QColor EqGraphWidget::band_hue(int index) {
    static const QColor hues[6] = {
        QColor(0xFF, 0x45, 0x3A), QColor(0xFF, 0x9F, 0x0A),
        QColor(0xFF, 0xD6, 0x0A), QColor(0x64, 0xD2, 0xFF),
        QColor(0x0A, 0x84, 0xFF), QColor(0xBF, 0x5A, 0xF2),
    };
    return hues[std::clamp(index, 0, 5)];
}

void EqGraphWidget::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const QRectF r = plot_rect();
    constexpr double kMinDb = -24.0;
    constexpr double kMaxDb = 24.0;
    constexpr double kMinHz = 20.0;
    constexpr double kMaxHz = 20000.0;

    const auto x_for = [&](double hz) {
        const double lg = std::log(hz / kMinHz) / std::log(kMaxHz / kMinHz);
        return r.left() + lg * r.width();
    };
    const auto y_for = [&](double db) {
        const double f = (std::clamp(db, kMinDb, kMaxDb) - kMinDb) / (kMaxDb - kMinDb);
        return r.bottom() - f * r.height();
    };

    if (view_ == View::Bands) {
        paint_bands(p, r, y_for);
        return;
    }

    // Grid: reference Hz ticks + 0 dB axis.
    const ThemeTokens& t = tokens();
    p.setPen(QPen(t.border_soft, 1));
    for (const double hz : {62.0, 250.0, 1000.0, 4000.0, 16000.0}) {
        const double x = x_for(hz);
        p.drawLine(QPointF(x, r.top()), QPointF(x, r.bottom()));
    }
    p.setPen(QPen(t.border, 1));
    p.drawLine(QPointF(r.left(), y_for(0.0)), QPointF(r.right(), y_for(0.0)));

    // Axes labels.
    p.setPen(t.ink_faint);
    p.setFont(QFont(QStringLiteral("DejaVu Sans"), 7));
    p.drawText(QPointF(r.left() + 1, r.bottom() - 1), QStringLiteral("20"));
    p.drawText(QPointF(r.right() - 14, r.bottom() - 1), QStringLiteral("20K"));
    // dB ticks sit BELOW the plot's bottom and top edges, in the margins
    // plot_rect() reserves (the old 6 px margin clipped the bottom one).
    p.drawText(QPointF(r.left() + 1, y_for(-24.0) + 6), QStringLiteral("-24"));
    p.drawText(QPointF(r.left() + 1, y_for(24.0) - 10), QStringLiteral("+24"));

    // Response curve: the TRUE 6-band cascade magnitude, sampled on a log
    // grid at the canonical 48 kHz pipeline rate. The equalizer law is
    // rate-independent across the display band (see equalizer.hpp), so the
    // plot matches what every playback/export rate actually runs. Disabled
    // bands are excluded by equalizer_response() itself (it shares
    // band_filters() with the DSP cascade).
    //
    // Per-band translucent fills (FreeEQ8 style) sit under the composite:
    // each enabled band's OWN biquad response, tinted its accent hue and
    // filled to the 0 dB line. This makes the interacted-with band's
    // contribution readable at a glance (selected band glows brighter)
    // instead of showing only the cascade sum.
    {
        constexpr int kSamples = 160;
        for (int bi = 0; bi < static_cast<int>(bands_.size()); ++bi) {
            const auto& b = bands_[bi];
            if (!b.enabled) continue;
            const bool sel = bi == selected_;
            const double zero_y = y_for(0.0);
            QPainterPath fill;
            bool started = false;
            for (int i = 0; i <= kSamples; ++i) {
                const double hz = kMinHz * std::pow(kMaxHz / kMinHz,
                                                    static_cast<double>(i) / kSamples);
                const double db = canvas::core::equalizer_band_response(b, 48000, hz);
                const QPointF pt(x_for(hz), y_for(db));
                if (!started) {
                    fill.moveTo(pt);
                    started = true;
                } else {
                    fill.lineTo(pt);
                }
            }
            fill.lineTo(QPointF(x_for(kMaxHz), zero_y));
            fill.lineTo(QPointF(x_for(kMinHz), zero_y));
            fill.closeSubpath();
            const QColor hue = band_hue(bi);
            p.setPen(Qt::NoPen);
            p.setBrush(with_alpha(hue, sel ? 38 : 15));
            p.drawPath(fill);
        }
    }
    QPainterPath path;
    {
        constexpr int kSamples = 160;
        bool first = true;
        for (int i = 0; i <= kSamples; ++i) {
            const double hz = kMinHz * std::pow(kMaxHz / kMinHz,
                                                static_cast<double>(i) / kSamples);
            const double db = canvas::core::equalizer_response(bands_, 48000, hz);
            const QPointF pt(x_for(hz), y_for(db));
            if (first) {
                path.moveTo(pt);
                first = false;
            } else {
                path.lineTo(pt);
            }
        }
    }
    p.setPen(QPen(t.accent, 2));
    p.drawPath(path);

    // Band nodes: numbered, filled when enabled, hollow + dim when
    // bypassed. FreeEQ8-style: the selected node gets a soft glow, the
    // hovered node a highlight ring, and each node carries its band number
    // inside so the graph rows and the fill hues share one identity. LP/HP
    // nodes draw their vertical handle at the locked-gain line (they have
    // no gain knob, matching the greyed row spinbox).
    constexpr double kNodeR = 6.0;
    constexpr double kHitR = 12.0;
    const auto node_gain = [&](const canvas::core::Clip::EqBand& b) {
        return gain_locked(b) ? 0.0 : static_cast<double>(b.gain);
    };
    for (int i = 0; i < static_cast<int>(bands_.size()); ++i) {
        const auto& b = bands_[i];
        const QPointF c(x_for(b.frequency), y_for(node_gain(b)));
        const QColor hue = band_hue(i);
        const bool sel = i == selected_;
        const bool hot = i == hovered_;
        if (sel) {  // soft glow behind the selected node
            p.setPen(Qt::NoPen);
            p.setBrush(with_alpha(hue, 26));
            p.drawEllipse(c, kNodeR * 2.4, kNodeR * 2.4);
        }
        if (hot && !sel) {  // hover highlight ring before the node body
            p.setPen(QPen(with_alpha(hue, 170), 1.2));
            p.setBrush(Qt::NoBrush);
            p.drawEllipse(c, kNodeR + 3.0, kNodeR + 3.0);
        }
        p.setBrush(b.enabled ? hue : Qt::NoBrush);
        p.setPen(QPen(b.enabled ? hue : with_alpha(hue, 120), b.enabled ? 2.0 : 1.2));
        p.drawEllipse(c, kNodeR, kNodeR);
        // White hairline outline: lifts the drag point off the grid lines
        // and the response curve so it reads as a handle at a glance, in
        // both the filled and hollow (bypassed) states.
        if (b.enabled) {
            p.setPen(QPen(with_alpha(t.ink, 90), 1.0));
            p.setBrush(Qt::NoBrush);
            p.drawEllipse(c, kNodeR - 1.0, kNodeR - 1.0);
        }
        if (b.enabled) {  // band number inside the node
            p.setBrush(Qt::NoBrush);
            p.setPen(t.surface_low);
            // Panel font scaled to the node, metric-centered inside the
            // drawn circle (not a hardcoded family) so the digit reads
            // crisp at any theme/dpi and never clips.
            QFont f = this->font();
            const double kScale = 0.56;  // of the panel text height
            f.setPointSizeF(f.pointSizeF() * kScale);
            f.setBold(true);
            const QString label = QStringLiteral("%1").arg(i + 1);
            const QRectF tr = QFontMetricsF(f).boundingRect(label);
            p.setFont(f);
            p.drawText(QPointF(c.x() - tr.width() / 2.0 - tr.x(),
                               c.y() - tr.height() / 2.0 - tr.y()),
                       label);
        }
        if (sel) {  // selection ring (not a second glow)
            p.setPen(QPen(t.accent, 1.4));
            p.setBrush(Qt::NoBrush);
            p.drawEllipse(c, kNodeR + kHitR * 0.5, kNodeR + kHitR * 0.5);
        }
    }
}

void EqGraphWidget::paint_bands(QPainter& p, const QRectF& r,
                                const std::function<double(double)>& y_for) {
    const ThemeTokens& t = tokens();
    constexpr double kMinDb = -24.0;
    constexpr double kMaxDb = 24.0;

    // 0 dB reference line spans all columns.
    p.setPen(QPen(t.border, 1));
    p.drawLine(QPointF(r.left(), y_for(0.0)), QPointF(r.right(), y_for(0.0)));

    const int count = static_cast<int>(bands_.size());
    const double col_w = r.width() / count;
    const double gap = 4.0;
    for (int i = 0; i < count; ++i) {
        const auto& b = bands_[i];
        const bool sel = i == selected_;
        const bool locked = gain_locked(b);
        const double g = locked ? 0.0 : std::clamp(static_cast<double>(b.gain),
                                                   kMinDb, kMaxDb);
        const QColor hue = band_hue(i);
        const QRectF col(r.left() + i * col_w + gap / 2, r.top(),
                         col_w - gap, r.height());

        // Column track background (accent-tinted when selected).
        p.setPen(Qt::NoPen);
        p.setBrush(sel ? with_alpha(t.accent, 14) : with_alpha(t.surface_higher, 90));
        p.drawRoundedRect(col, 3, 3);

        // Gain fill from the 0 dB line to the band gain.
        const double y0 = y_for(0.0);
        const double yg = y_for(g);
        const double top = std::min(y0, yg);
        const double h = std::abs(yg - y0);
        if (h > 0.5 && b.enabled) {
            p.setBrush(with_alpha(hue, locked ? 90 : 150));
            p.setPen(Qt::NoPen);
            p.drawRoundedRect(QRectF(col.left() + 1, top, col.width() - 2, h), 2, 2);
        }

        // Gain handle at the current level.
        p.setPen(b.enabled ? QPen(hue, 2.0) : QPen(with_alpha(hue, 110), 1.2));
        p.setBrush(b.enabled ? hue : Qt::NoBrush);
        const QPointF hc(col.center().x(), yg);
        p.drawEllipse(hc, 4.5, 4.5);
        if (sel) {
            p.setPen(QPen(t.accent, 1.4));
            p.setBrush(Qt::NoBrush);
            p.drawEllipse(hc, 4.5 + 3.2, 4.5 + 3.2);
        }

        // Enable dot at the column top (mirrors the row dot).
        const QPointF dot(col.center().x(), r.top() + 7);
        p.setPen(b.enabled ? QPen(hue, 1.4) : QPen(with_alpha(hue, 100), 1.2));
        p.setBrush(b.enabled ? hue : Qt::NoBrush);
        p.drawEllipse(dot, 3.0, 3.0);
    }

    // Band labels under the handle... drawn below the plot as a footer so
    // the columns keep all their height; the row numbers stay in sync.
    p.setPen(t.ink_faint);
    p.setFont(QFont(QStringLiteral("DejaVu Sans"), 7));
    for (int i = 0; i < count; ++i) {
        const double cx = r.left() + (i + 0.5) * col_w;
        p.drawText(QPointF(cx - 6, rect().bottom() - 2),
                   QStringLiteral("B%1").arg(i + 1));
    }
}

int EqGraphWidget::column_at(double px) const {
    const QRectF r = plot_rect();
    if (px < r.left() || px > r.right()) return -1;
    const int count = static_cast<int>(bands_.size());
    const double col_w = r.width() / count;
    const int i = static_cast<int>((px - r.left()) / col_w);
    return (i >= 0 && i < count) ? i : -1;
}

void EqGraphWidget::mousePressEvent(QMouseEvent* e) {
    if (e->button() != Qt::LeftButton) return;
    const int idx = view_ == View::Bands ? column_at(e->position().x())
                                         : node_at(e->position());
    if (idx < 0) {
        // Clicking the empty plot clears the selection.
        set_selected(-1);
        return;
    }
    // Any in-flight wheel commit is superseded by the drag that just began.
    wheel_timer_.stop();
    hovered_ = -1;  // the drag owns the highlight now
    set_selected(idx);
    dragging_ = true;
    drag_band_ = idx;
    setCursor(Qt::ClosedHandCursor);
}

void EqGraphWidget::mouseMoveEvent(QMouseEvent* e) {
    const QPointF pos = e->position();
    if (!dragging_ || drag_band_ < 0) {
        const int idx = view_ == View::Bands ? column_at(pos.x())
                                             : node_at(pos);
        if (idx != hovered_) {
            hovered_ = idx;  // hover highlight on the curve nodes
            if (view_ == View::Curve) update();
        }
        setCursor(idx >= 0 ? Qt::OpenHandCursor : Qt::ArrowCursor);
        return;
    }
    auto& b = bands_[drag_band_];
    const QRectF r = plot_rect();
    // Node drag is pinned INSIDE the plot: the pointer maps into the plot
    // with a small margin so the dot (+ its white ring + selection ring)
    // never ends up dead on the axis ticks or the "20"/"20K"/dB labels.
    // The curve stays fully draggable across its range — only the LAST
    // ~4px of each edge are reserved so the paint never clips the handle.
    const double kEdgeInset = 4.0;
    const QRectF drag_r = r.adjusted(kEdgeInset, kEdgeInset, -kEdgeInset, -kEdgeInset);
    const auto inv_x = [&](double px) {
        const double lg = std::clamp((px - drag_r.left()) / drag_r.width(), 0.0, 1.0);
        return kMinHzV() * std::pow(kMaxHzV() / kMinHzV(), lg);
    };
    const auto inv_y = [&](double py) {
        const double f = std::clamp((drag_r.bottom() - py) / drag_r.height(), 0.0, 1.0);
        return kMinDbV() + f * (kMaxDbV() - kMinDbV());
    };

    if (view_ == View::Bands) {
        // Faders view: the vertical axis is the only control; the
        // horizontal position within the column is meaningless.
        if (gain_locked(b)) return;
        const double gain =
            std::clamp(inv_y(pos.y()), kMinDbV(), kMaxDbV());
        if (gain == b.gain) return;
        b.gain = static_cast<float>(gain);
        update();
        if (on_edit) on_edit(drag_band_);
        return;
    }

    const bool gain_only = e->modifiers() & (Qt::ControlModifier | Qt::AltModifier);
    const bool freq_only = e->modifiers() & Qt::ShiftModifier;
    // Which axes may actually change this move.
    const bool freq_writable = !gain_only;
    const bool gain_writable = !freq_only && !gain_locked(b);

    double freq = b.frequency;
    double gain = b.gain;
    if (freq_writable) freq = inv_x(pos.x());
    if (gain_writable) gain = inv_y(pos.y());
    freq = std::clamp(freq, static_cast<double>(canvas::core::audio_processing::kEqFreqMin),
                      static_cast<double>(canvas::core::audio_processing::kEqFreqMax));
    gain = std::clamp(gain, static_cast<double>(canvas::core::audio_processing::kEqGainMin),
                      static_cast<double>(canvas::core::audio_processing::kEqGainMax));
    if ((freq_writable && freq != b.frequency) ||
        (gain_writable && gain != b.gain)) {
        if (freq_writable) b.frequency = static_cast<float>(freq);
        if (gain_writable) b.gain = static_cast<float>(gain);
        update();
        if (on_edit) on_edit(drag_band_);
    }
}

void EqGraphWidget::mouseReleaseEvent(QMouseEvent* e) {
    if (e->button() != Qt::LeftButton) return;
    if (dragging_) {
        dragging_ = false;
        setCursor(Qt::ArrowCursor);
        if (on_commit) on_commit();
    }
}

void EqGraphWidget::leaveEvent(QEvent*) {
    // Clear the hover highlight so a ring never lingers when the cursor
    // leaves the widget (also covers mid-gesture exits).
    if (hovered_ >= 0) {
        hovered_ = -1;
        update();
    }
}

void EqGraphWidget::mouseDoubleClickEvent(QMouseEvent* e) {
    const auto pos = e->position();
    const int idx = view_ == View::Bands ? column_at(pos.x()) : node_at(pos);
    if (idx < 0) return;
    bands_[idx].enabled = !bands_[idx].enabled;
    set_selected(idx);
    update();
    if (on_edit) on_edit(idx);
    // Double-click = one discrete edit → commit immediately.
    if (on_commit) on_commit();
}

void EqGraphWidget::wheelEvent(QWheelEvent* e) {
    // Wheel targets the band under the cursor, else the selected band.
    const auto pos = e->position();
    int idx = view_ == View::Bands ? column_at(pos.x()) : node_at(pos);
    if (idx < 0) idx = selected_;
    if (idx < 0) return;
    auto& b = bands_[idx];
    const double steps = static_cast<double>(e->angleDelta().y()) / 120.0;
    const double lo = canvas::core::audio_processing::kEqQMin;
    const double hi = canvas::core::audio_processing::kEqQMax;
    // Log Q steps: ~1.15x per notch, so a full sweep 0.1→10 is ~32 notches.
    const double scale = std::pow(1.15, steps);
    const double nq = std::clamp(static_cast<double>(b.q) * scale, lo, hi);
    if (nq != b.q) {
        b.q = static_cast<float>(nq);
        set_selected(idx);
        update();
        if (on_edit) on_edit(idx);
        wheel_timer_.start();  // debounced single commit
    }
}

QRectF EqGraphWidget::plot_rect() const {
    // Asymmetric margins sized so NOTHING painted on top of the plot —
    // the "+24"/"-24" axis ticks, the "20"/"20K" freq labels, the node
    // circles, and the selected node's 2.4x soft glow (drawn up to ~14 px
    // beyond the node center) — ever clips against the widget edge. The
    // left edge also hosts the dB ticks, the right edge the "20K" label.
    return rect().adjusted(16, 22, -20, -26);
}

int EqGraphWidget::node_at(const QPointF& pos) const {
    const QRectF r = plot_rect();
    const auto x_for = [&](double hz) {
        const double lg = std::log(hz / kMinHzV()) / std::log(kMaxHzV() / kMinHzV());
        return r.left() + lg * r.width();
    };
    const auto y_for = [&](double db) {
        const double f = (std::clamp(db, kMinDbV(), kMaxDbV()) - kMinDbV()) /
                         (kMaxDbV() - kMinDbV());
        return r.bottom() - f * r.height();
    };
    int best = -1;
    double best_d = 12.0;  // kHitR, matching the painted hit radius
    for (int i = 0; i < static_cast<int>(bands_.size()); ++i) {
        const auto& b = bands_[i];
        const double y = y_for(gain_locked(b) ? 0.0 : static_cast<double>(b.gain));
        const QPointF c(x_for(b.frequency), y);
        const double d = std::hypot(pos.x() - c.x(), pos.y() - c.y());
        if (d < best_d) {
            best_d = d;
            best = i;
        }
    }
    return best;
}

bool EqGraphWidget::gain_locked(const canvas::core::Clip::EqBand& b) {
    return b.type == canvas::core::Clip::EqBand::Type::LowPass ||
           b.type == canvas::core::Clip::EqBand::Type::HighPass;
}

void EqGraphWidget::emit_selection() {
    if (on_selection_changed) on_selection_changed(selected_);
}

}  // namespace canvas::gui