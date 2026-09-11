#include "UX/InspectorAudio.hpp"

#include "UX/InspectorShared.hpp"
#include "UX/MainWindow.hpp"

#include "canvas/core/media/equalizer.hpp"
#include "canvas/core/timeline/audio_mix.hpp"
#include "canvas/core/timeline/audio_processing.hpp"
#include "canvas/core/timeline/edit_ops.hpp"

#include <QButtonGroup>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QSlider>
#include <QStandardItemModel>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>
#include <QWheelEvent>
#include <QWidget>

#include <algorithm>
#include <cmath>
#include <functional>
#include <map>
#include <vector>

#include "Widgets/timeline_widget.hpp"
#include "features/timeline/audio_targets.hpp"

namespace canvas::gui {

namespace {

// ── EQ response graph ─────────────────────────────────────────────────────
// A hand-painted frequency-response plot: x = Hz (log), y = dB (-24..+24).
// The curve is the TRUE 6-band cascade magnitude (RBJ biquads via the shared
// headless law canvas::core::equalizer_response) — the same math the
// playback/export DSP runs, so what the plot shows is exactly what the clip
// filters. The band markers sit at each band's nominal (type/freq/gain/Q)
// params and are purely handles — the curve already reflects their real
// interaction.
//
// Two views (View toggle above the category):
//   Curve — the classic node graph. Interaction model (Resolve-style):
//   • LMB-drag a node = frequency + gain (joint). Hold Shift → frequency only;
//     hold Ctrl/Alt → gain only.
//   • Mouse wheel over a node (or over the selected band) = Q, in log steps.
//   • Click a node to select it (echoed by a ring + the numeric row highlight).
//   • Double-click a node = toggle that band's enable (hollow/dim + dropped
//     from the cascade curve). Same rule drives the row's enable dot.
//   • LowPass/HighPass have no meaningful gain — vertical drag is refused for
//     them and the node stays pinned to its (locked) gain line.
//   Bands — one vertical gain fader per band arranged as a row of columns,
//   the layout EasyEffects uses. Drag a column = that band's gain (the only
//   axis); wheel = Q; the dot atop each column toggles the band.
//   • Live band edits stream through `on_edit`; a drag release / double-click /
//     wheel settle calls `on_commit` exactly once, so each gesture is one undoable
//     audio-processing edit.
class EqGraphWidget final : public QWidget {
public:
    using Bands = std::array<canvas::core::Clip::EqBand, 6>;

    // Two render modes, switched by the View toggle above the category:
    //   Curve       — classic node graph on the log-frequency axis (drag
    //                 joints / Shift / Ctrl-Alt, wheel = Q). Default.
    //   EasyEffects — one vertical gain fader per band, arranged as a row of
    //                 columns exactly like EasyEffects' band columns. Dragging
    //                 a fader rides a single band's gain; the Q knob still
    //                 follows the wheel; the dot on top toggles the band.
    enum class View : int { Curve = 0, Bands = 1 };

    explicit EqGraphWidget(QWidget* parent = nullptr) : QWidget(parent) {
        bands_ = canvas::core::Clip::default_eq_bands();
        setMinimumHeight(150);
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

    void set_bands(const Bands& bands) {
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

    const Bands& bands() const { return bands_; }
    int selected() const { return selected_; }

    View view() const { return view_; }
    void set_view(View v) {
        if (view_ == v) return;
        view_ = v;
        // The two views share the same bands, but a drag's axis mental-model
        // does not survive the switch — drop any in-flight gesture and return
        // to a plain arrow so a stale press across modes can't grab a fader.
        dragging_ = false;
        drag_band_ = -1;
        wheel_timer_.stop();
        setCursor(Qt::ArrowCursor);
        update();
    }

    // Selection echo → row highlight + clear-on-commit state.
    void set_selected(int index) {
        selected_ = index;
        update();
        emit_selection();
    }

    // Callbacks wired by build_inspector_audio. `on_edit` fires on every live
    // band change (drag/wheel/type/enable via the graph); `on_commit` fires
    // once when a gesture settles and wants one undoable edit recorded.
    std::function<void(int index)> on_edit;
    std::function<void()> on_commit;
    std::function<void(int index)> on_selection_changed;

    // Per-band accent hues, shared by the graph nodes and the row labels so the
    // two never drift.
    static QColor band_hue(int index) {
        static const QColor hues[6] = {
            QColor(0xF2, 0x6D, 0x5B), QColor(0xF7, 0xB7, 0x33),
            QColor(0xBF, 0xD6, 0x3C), QColor(0x59, 0xC2, 0x8D),
            QColor(0x47, 0xB5, 0xDE), QColor(0x8C, 0x8F, 0xEE),
        };
        return hues[std::clamp(index, 0, 5)];
    }

protected:
    void paintEvent(QPaintEvent*) override {
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
        p.drawText(QPointF(r.left() + 1, y_for(-24.0) + 6), QStringLiteral("-24"));
        p.drawText(QPointF(r.left() + 1, y_for(24.0) - 2), QStringLiteral("+24"));

        // Response curve: the TRUE 6-band cascade magnitude, sampled on a log
        // grid at the canonical 48 kHz pipeline rate. The equalizer law is
        // rate-independent across the display band (see equalizer.hpp), so the
        // plot matches what every playback/export rate actually runs. Disabled
        // bands are excluded by equalizer_response() itself (it shares
        // band_filters() with the DSP cascade).
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

        // Band nodes: filled when enabled, hollow + dim when bypassed. The
        // selected node gets an accent ring; LP/HP nodes draw their vertical
        // handle at the locked-gain line (they have no gain knob, matching the
        // greyed row spinbox). The node hue doubles as the band identity shared
        // with the numeric rows.
        constexpr double kNodeR = 4.5;
        constexpr double kHitR = 9.0;
        const auto node_gain = [&](const canvas::core::Clip::EqBand& b) {
            return gain_locked(b) ? 0.0 : static_cast<double>(b.gain);
        };
        for (int i = 0; i < static_cast<int>(bands_.size()); ++i) {
            const auto& b = bands_[i];
            const QPointF c(x_for(b.frequency), y_for(node_gain(b)));
            const QColor hue = band_hue(i);
            p.setBrush(b.enabled ? hue : Qt::NoBrush);
            p.setPen(QPen(b.enabled ? hue : with_alpha(hue, 120), b.enabled ? 2.0 : 1.2));
            p.drawEllipse(c, kNodeR, kNodeR);
            if (i == selected_) {
                p.setPen(QPen(t.accent, 1.4));
                p.setBrush(Qt::NoBrush);
                p.drawEllipse(c, kNodeR + kHitR * 0.6, kNodeR + kHitR * 0.6);
            }
        }
    }

    void paint_bands(QPainter& p, const QRectF& r,
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

    [[nodiscard]] int column_at(double px) const {
        const QRectF r = plot_rect();
        if (px < r.left() || px > r.right()) return -1;
        const int count = static_cast<int>(bands_.size());
        const double col_w = r.width() / count;
        const int i = static_cast<int>((px - r.left()) / col_w);
        return (i >= 0 && i < count) ? i : -1;
    }

    void mousePressEvent(QMouseEvent* e) override {
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
        set_selected(idx);
        dragging_ = true;
        drag_band_ = idx;
        setCursor(Qt::ClosedHandCursor);
    }

    void mouseMoveEvent(QMouseEvent* e) override {
        const QPointF pos = e->position();
        if (!dragging_ || drag_band_ < 0) {
            const int idx = view_ == View::Bands ? column_at(pos.x())
                                                 : node_at(pos);
            setCursor(idx >= 0 ? Qt::OpenHandCursor : Qt::ArrowCursor);
            return;
        }
        auto& b = bands_[drag_band_];
        const QRectF r = plot_rect();
        const auto inv_x = [&](double px) {
            const double lg = std::clamp((px - r.left()) / r.width(), 0.0, 1.0);
            return kMinHzV() * std::pow(kMaxHzV() / kMinHzV(), lg);
        };
        const auto inv_y = [&](double py) {
            const double f = std::clamp((r.bottom() - py) / r.height(), 0.0, 1.0);
            return kMinDbV() + f * (kMaxDbV() - kMinDbV());
        };

        if (view_ == View::Bands) {
            // EasyEffects columns: the vertical axis is the only control; the
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

    void mouseReleaseEvent(QMouseEvent* e) override {
        if (e->button() != Qt::LeftButton) return;
        if (dragging_) {
            dragging_ = false;
            setCursor(Qt::ArrowCursor);
            if (on_commit) on_commit();
        }
    }

    void mouseDoubleClickEvent(QMouseEvent* e) override {
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

    void wheelEvent(QWheelEvent* e) override {
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

private:
    [[nodiscard]] static constexpr double kMinHzV() { return 20.0; }
    [[nodiscard]] static constexpr double kMaxHzV() { return 20000.0; }
    [[nodiscard]] static constexpr double kMinDbV() { return -24.0; }
    [[nodiscard]] static constexpr double kMaxDbV() { return 24.0; }

    [[nodiscard]] QRectF plot_rect() const { return rect().adjusted(6, 6, -6, -6); }

    // The band index under `pos` (nearest node within the hit radius), or -1.
    [[nodiscard]] int node_at(const QPointF& pos) const {
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
        double best_d = 9.0;
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

    // LP/HP have no gain knob: the node is pinned to the 0 dB line and vertical
    // drag is refused (both here and via the greyed row spinbox).
    [[nodiscard]] static bool gain_locked(const canvas::core::Clip::EqBand& b) {
        return b.type == canvas::core::Clip::EqBand::Type::LowPass ||
               b.type == canvas::core::Clip::EqBand::Type::HighPass;
    }

    void emit_selection() {
        if (on_selection_changed) on_selection_changed(selected_);
    }

    Bands bands_ = canvas::core::Clip::default_eq_bands();
    View view_ = View::Curve;
    int selected_ = -1;
    int drag_band_ = -1;
    bool dragging_ = false;
    QTimer wheel_timer_;
};

// ── Slider + numeric spin composite row ───────────────────────────────────
QWidget* make_slider_spin(double min, double max, int decimals, QWidget* parent,
                          QSlider** out_slider, QDoubleSpinBox** out_spin) {
    auto* host = new QWidget(parent);
    auto* lay = new QHBoxLayout(host);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->setSpacing(6);
    auto* slider = new QSlider(Qt::Horizontal, host);
    slider->setRange(0, 10000);
    // Don't let the slider's default minimum width (≈84px) force the row wider
    // than the inspector dock — the label/spin/reset on the right must stay
    // visible at any dock width.
    slider->setMinimumWidth(0);
    auto* spin = new QDoubleSpinBox(host);
    spin->setRange(min, max);
    spin->setDecimals(decimals);
    spin->setMaximumWidth(74);
    spin->setKeyboardTracking(false);

    const auto spin_to_slider = [slider, min, max](double v) {
        slider->setValue(static_cast<int>(std::lround((v - min) / (max - min) * 10000.0)));
    };
    const auto slider_to_spin = [spin, min, max](int v) {
        spin->setValue(min + (max - min) * static_cast<double>(v) / 10000.0);
    };
    QObject::connect(spin, &QDoubleSpinBox::valueChanged, host, spin_to_slider);
    QObject::connect(slider, &QSlider::valueChanged, host, slider_to_spin);

    QObject::connect(spin, &QDoubleSpinBox::editingFinished, host,
                     [spin, slider, min, max]() {
                         spin->setValue(min + (max - min) *
                                            static_cast<double>(slider->value()) / 10000.0);
                     });

    // Seed the knob to match the spin's initial value. The spin starts at its
    // built-in default (0.0) and the slider at its own default (0 = far-left on
    // the 0..10000 range), so without this they disagree until the user touches
    // the spin — e.g. a -100..+100 dB volume row would show 0.00 with the knob
    // hard against the left end instead of centered.
    spin_to_slider(spin->value());

    lay->addWidget(slider, 1);
    lay->addWidget(spin);
    if (out_slider) *out_slider = slider;
    if (out_spin) *out_spin = spin;
    return host;
}

QComboBox* make_dark_combo(QWidget* parent) {
    auto* cb = new QComboBox(parent);
    apply_theme_style(cb, [] {
        const ThemeTokens& t = tokens();
        return QStringLiteral(
                   "QComboBox { background-color: %1; color: %2; border: 1px solid %3;"
                   "  border-radius: 8px; padding: 3px 8px; font-size: 11px; }"
                   "QComboBox::drop-down { border: none; width: 14px; }"
                   "QComboBox QAbstractItemView { background-color: %4; color: %2;"
                   "  selection-background-color: %5; border: 1px solid %3;"
                   "  border-radius: 8px; padding: 2px; }")
            .arg(css(t.surface_higher), css(t.ink), css(t.border), css(t.surface_low),
                 css(t.accent));
    });
    return cb;
}

QDoubleSpinBox* make_band_spin(double lo, double hi, int decimals, double val, QWidget* parent,
                               int width) {
    auto* s = new QDoubleSpinBox(parent);
    s->setRange(lo, hi);
    s->setValue(val);
    s->setDecimals(decimals);
    s->setMaximumWidth(width);
    s->setKeyboardTracking(false);
    apply_theme_style(s, [] {
        const ThemeTokens& t = tokens();
        return QStringLiteral(
                   "QDoubleSpinBox { background-color: %1; color: %2; border: 1px solid %3;"
                   "  border-radius: 8px; padding: 3px 8px; font-size: 11px; }")
            .arg(css(t.surface_higher), css(t.ink), css(t.border));
    });
    return s;
}

// ── Control registry ──────────────────────────────────────────────────────
struct AudioControls {
    QDoubleSpinBox* volume = nullptr;
    QSlider* volume_slider = nullptr;
    QDoubleSpinBox* pan = nullptr;
    QSlider* pan_slider = nullptr;
    QLabel* multi_hint = nullptr;

    QSlider* pitch_semi_slider = nullptr;
    QDoubleSpinBox* pitch_semi = nullptr;
    QSlider* pitch_cents_slider = nullptr;
    QDoubleSpinBox* pitch_cents = nullptr;

    InspectorCategory* speed_cat = nullptr;
    QSlider* speed_slider = nullptr;
    QDoubleSpinBox* speed_factor = nullptr;

    InspectorCategory* eq_cat = nullptr;
    EqGraphWidget* eq_graph = nullptr;
    QButtonGroup* eq_view_group = nullptr;  // Curve / EasyEffects view switch
    QToolButton* eq_view_curve = nullptr;
    QToolButton* eq_view_bands = nullptr;
    std::vector<QDoubleSpinBox*> eq_freq;
    std::vector<QDoubleSpinBox*> eq_gain;
    std::vector<QDoubleSpinBox*> eq_q;
    std::vector<QComboBox*> eq_type;
    std::vector<QToolButton*> eq_enable;  // per-band bypass dots (mirror EqBand.enabled)
    std::vector<QLabel*> eq_labels;       // B1..B6 labels, recoloured on selection

    // AI Voice Isolation: per-clip engine picker. Backed by the real model
    // field (None / RNNoise / DeepFilterNet) and applied before the clip's
    // gains/mix in both playback and export. Engine entries whose backend is
    // not compiled in (voice_isolation_supported()==false, i.e. DeepFilterNet)
    // are listed but disabled.
    InspectorCategory* iso_cat = nullptr;
    QComboBox* iso_combo = nullptr;

    InspectorCategory* ai_leveler = nullptr;
    InspectorCategory* ai_remix = nullptr;
    QDoubleSpinBox* ai_amount = nullptr;

    QToolButton* mode_button = nullptr;
    bool updating = false;   // guards against commit/re-sync during refresh
    bool attached = false;   // selection signals already connected
};

std::map<MainWindow*, AudioControls>& audio_registry() {
    static std::map<MainWindow*, AudioControls> reg;
    return reg;
}

AudioControls* audio_lookup(MainWindow& mw) {
    const auto it = audio_registry().find(&mw);
    return it == audio_registry().end() ? nullptr : &it->second;
}

void set_processing_enabled(AudioControls& ac, bool on) {
    for (QDoubleSpinBox* s : {ac.pitch_semi, ac.pitch_cents, ac.speed_factor})
        if (s) s->setEnabled(on);
    for (QSlider* s : {ac.pitch_semi_slider, ac.pitch_cents_slider, ac.speed_slider})
        if (s) s->setEnabled(on);
    for (QDoubleSpinBox* s : ac.eq_freq) s->setEnabled(on);
    for (QDoubleSpinBox* s : ac.eq_gain) s->setEnabled(on);
    for (QDoubleSpinBox* s : ac.eq_q) s->setEnabled(on);
    for (QComboBox* c : ac.eq_type) c->setEnabled(on);
    for (QToolButton* b : ac.eq_enable) if (b) b->setEnabled(on);
    if (ac.eq_view_group) {
        for (QAbstractButton* b : ac.eq_view_group->buttons()) b->setEnabled(on);
    }
    if (ac.eq_graph) {
        ac.eq_graph->setEnabled(on);
        // LP/HP rows have no gain control — greyed even when the section is on.
        for (std::size_t i = 0; i < ac.eq_type.size() && i < ac.eq_gain.size(); ++i) {
            if (!ac.eq_type[i] || !ac.eq_gain[i]) continue;
            ac.eq_gain[i]->setEnabled(
                on && ac.eq_type[i]->currentIndex() !=
                          static_cast<int>(canvas::core::Clip::EqBand::Type::LowPass) &&
                ac.eq_type[i]->currentIndex() !=
                    static_cast<int>(canvas::core::Clip::EqBand::Type::HighPass));
        }
    }
    if (ac.iso_combo) ac.iso_combo->setEnabled(on);
    for (InspectorCategory* cat : {ac.speed_cat, ac.eq_cat, ac.iso_cat})
        if (cat) {
            cat->set_feature_toggle_enabled(on);
            cat->setEnabled(on);
        }
}

// Reads a freshly-found selected audio clip into the widgets (no commit).
void populate_from_clip(AudioControls& ac, const canvas::core::Clip& clip) {
    ac.updating = true;
    if (ac.volume) ac.volume->setValue(clip.volume_db);
    if (ac.pan) ac.pan->setValue(clip.pan);
    if (ac.pitch_semi) ac.pitch_semi->setValue(clip.pitch_semitones);
    if (ac.pitch_cents) ac.pitch_cents->setValue(clip.pitch_cents);
    if (ac.speed_factor) ac.speed_factor->setValue(clip.speed_factor);
    if (ac.speed_cat) ac.speed_cat->set_feature_enabled(clip.speed_enabled);
    if (ac.eq_cat) ac.eq_cat->set_feature_enabled(clip.eq_enabled);
    for (std::size_t i = 0; i < clip.eq_bands.size(); ++i) {
        const auto& b = clip.eq_bands[i];
        if (i < ac.eq_type.size() && ac.eq_type[i])
            ac.eq_type[i]->setCurrentIndex(static_cast<int>(b.type));
        if (i < ac.eq_freq.size() && ac.eq_freq[i]) ac.eq_freq[i]->setValue(b.frequency);
        if (i < ac.eq_gain.size() && ac.eq_gain[i]) ac.eq_gain[i]->setValue(b.gain);
        if (i < ac.eq_q.size() && ac.eq_q[i]) ac.eq_q[i]->setValue(b.q);
        if (i < ac.eq_enable.size() && ac.eq_enable[i])
            ac.eq_enable[i]->setChecked(b.enabled);
    }
    if (ac.eq_graph) {
        ac.eq_graph->set_bands(clip.eq_bands);
        // Entering a new clip clears the node/row selection.
        ac.eq_graph->set_selected(-1);
    }
    if (ac.iso_combo)
        ac.iso_combo->setCurrentIndex(static_cast<int>(clip.voice_isolation));
    ac.updating = false;
}

}  // namespace

void build_inspector_audio(MainWindow& mw, QVBoxLayout* audio_layout,
                           QToolButton* audio_mode_button) {
    AudioControls& ac = audio_registry()[&mw];
    ac.mode_button = audio_mode_button;
    auto* host = audio_layout->parentWidget();
    const auto tr = [&](const char* s) { return MainWindow::tr(s); };

    // --- Audio (Volume / Pan) ------------------------------------------------
    auto* audio = new InspectorCategory(tr("Audio"), true, host);
    {
        QSlider* vol_slider = nullptr;
        QDoubleSpinBox* vol_spin = nullptr;
        auto* vol_row = make_slider_spin(canvas::core::audio_mix::kVolumeDbSliderMin,
                                         canvas::core::audio_mix::kVolumeDbSliderMax,
                                         1, host, &vol_slider, &vol_spin);
        ac.volume = vol_spin;
        ac.volume_slider = vol_slider;
        vol_spin->setSuffix(QStringLiteral(" dB"));
        vol_spin->setMaximumWidth(110);
        // Drag the slider left to lower, right to raise; commit once the drag
        // releases so playback keeps streaming between drag steps.
        QObject::connect(vol_slider, &QSlider::sliderReleased, &mw,
                         [&mw]() { mw.apply_inspector_audio(); });
        // Live spectrum feedback: every drag/typing step re-renders the selected
        // clip's waveform at the knob's gain (no undo/commit per step); the
        // release handler above still records the one real volume edit.
        QObject::connect(vol_spin, &QDoubleSpinBox::valueChanged, &mw,
                         [&mw](double vol_db) { mw.preview_inspector_volume(static_cast<float>(vol_db)); });
        add_property_row(audio->body_layout(), tr("Volume (dB)"), vol_row);
    }
    QSlider* pan_slider = nullptr;
    QDoubleSpinBox* pan_spin = nullptr;
    auto* pan_row = make_slider_spin(canvas::core::audio_mix::kPanMin,
                                     canvas::core::audio_mix::kPanMax,
                                     2, host, &pan_slider, &pan_spin);
    ac.pan = pan_spin;
    ac.pan_slider = pan_slider;
    pan_spin->setSuffix(QStringLiteral(" L/R"));
    pan_spin->setMaximumWidth(110);
    // Drag right to pan right, left to pan left; commit once the drag releases
    // so playback keeps streaming between drag steps.
    QObject::connect(pan_slider, &QSlider::sliderReleased, &mw,
                     [&mw]() { mw.apply_inspector_audio(); });
    add_property_row(audio->body_layout(), tr("Pan"), pan_row);
    {
        // Multi-clip selection hint: Volume/Pan edits below apply to every
        // selected audio clip (Phase 4). Hidden for single-clip selections.
        auto* hint = new QLabel(host);
        apply_theme_style(hint, [] {
            return QStringLiteral("color: %1; font-size: 10px; padding: 0 4px;")
                .arg(css(tokens().ink_muted));
        });
        hint->setWordWrap(true);
        hint->setVisible(false);
        ac.multi_hint = hint;
        audio->body_layout()->addWidget(hint);
    }
    audio_layout->addWidget(audio);
    audio_layout->addSpacing(2);
    // Keep MainWindow's legacy mix spins pointing at these so the pre-split
    // apply_inspector_audio() (volume/pan commit path) keeps working unchanged.
    mw.inspector_audio_volume_ = ac.volume;
    mw.inspector_audio_pan_ = ac.pan;

    // --- Pitch ----------------------------------------------------------------
    auto* pitch = new InspectorCategory(tr("Pitch"), true, host);
    QSlider* s1 = nullptr;
    QDoubleSpinBox* sp1 = nullptr;
    auto* semi_row = make_slider_spin(canvas::core::audio_processing::kPitchSemitonesMin,
                                      canvas::core::audio_processing::kPitchSemitonesMax,
                                      0, host, &s1, &sp1);
    ac.pitch_semi_slider = s1;
    ac.pitch_semi = sp1;
    ac.pitch_semi->setSuffix(QStringLiteral(" st"));
    add_property_row(pitch->body_layout(), tr("Semi Tones"), semi_row);
    QSlider* s2 = nullptr;
    QDoubleSpinBox* sp2 = nullptr;
    auto* cents_row = make_slider_spin(canvas::core::audio_processing::kPitchCentsMin,
                                       canvas::core::audio_processing::kPitchCentsMax,
                                       0, host, &s2, &sp2);
    ac.pitch_cents_slider = s2;
    ac.pitch_cents = sp2;
    ac.pitch_cents->setSuffix(QStringLiteral(" ct"));
    add_property_row(pitch->body_layout(), tr("Cents"), cents_row);
    audio_layout->addWidget(pitch);

    // --- Speed Change ---------------------------------------------------------
    ac.speed_cat = new InspectorCategory(tr("Speed Change"), false, /*has_enable=*/true, host);
    ac.speed_cat->set_feature_toggle_enabled(false);
    QSlider* s3 = nullptr;
    QDoubleSpinBox* sp3 = nullptr;
    auto* speed_row = make_slider_spin(canvas::core::audio_processing::kSpeedMin,
                                       canvas::core::audio_processing::kSpeedMax,
                                       2, host, &s3, &sp3);
    ac.speed_slider = s3;
    ac.speed_factor = sp3;
    QToolButton* speed_row_reset = nullptr;
    add_property_row(ac.speed_cat->body_layout(), tr("Factor"), speed_row,
                     /*with_reset=*/true, &speed_row_reset);
    audio_layout->addWidget(ac.speed_cat);

    // "Reset to default": the category-header reset AND the Factor-row reset
    // both restore the speed to 1.00 (disabled tempo) and commit one undoable
    // edit. spin->setValue() keeps the linked slider in sync.
    const auto reset_speed = [&mw]() {
        AudioControls* acc = audio_lookup(mw);
        if (!acc || !acc->speed_factor) return;
        acc->speed_factor->setValue(1.0);
        apply_inspector_audio_processing(mw);
    };
    if (auto* rb = ac.speed_cat->reset_button())
        QObject::connect(rb, &QToolButton::clicked, &mw, reset_speed);
    if (speed_row_reset)
        QObject::connect(speed_row_reset, &QToolButton::clicked, &mw, reset_speed);

    // --- Equalizer ------------------------------------------------------------
    // Open by default so the bands are immediately editable — no collapsed/
    // hidden-by-default state.
    ac.eq_cat = new InspectorCategory(tr("Equalizer"), /*expanded=*/true, /*has_enable=*/true, host);
    ac.eq_cat->set_feature_toggle_enabled(false);
    // The EQ section sits at the bottom of the audio tab and gets generous
    // spacing so every value/suffix stays fully visible at any dock width.
    ac.eq_cat->body_layout()->setSpacing(12);

    // View toggle: Curve (node graph) vs EasyEffects (band fader columns). A
    // lightweight segmented pair, identical to the page-bar pills in style.
    {
        auto* view_row = new QWidget(host);
        auto* view_lay = new QHBoxLayout(view_row);
        view_lay->setContentsMargins(0, 0, 0, 0);
        view_lay->setSpacing(4);
        auto* seg = new QWidget(view_row);
        auto* seg_lay = new QHBoxLayout(seg);
        seg_lay->setContentsMargins(0, 0, 0, 0);
        seg_lay->setSpacing(0);
        auto* curve_btn = new QToolButton(seg);
        curve_btn->setCheckable(true);
        curve_btn->setChecked(true);
        curve_btn->setText(tr("Curve"));
        auto* bands_btn = new QToolButton(seg);
        bands_btn->setCheckable(true);
        bands_btn->setText(tr("EasyEffects"));
        ac.eq_view_group = new QButtonGroup(seg);
        ac.eq_view_group->setExclusive(true);
        ac.eq_view_group->addButton(curve_btn, 0);
        ac.eq_view_group->addButton(bands_btn, 1);
        ac.eq_view_curve = curve_btn;
        ac.eq_view_bands = bands_btn;
        curve_btn->setFixedHeight(20);
        bands_btn->setFixedHeight(20);
        const auto seg_style = [] {
            const ThemeTokens& t = tokens();
            return QStringLiteral(
                       "QToolButton { background: %1; color: %2; border: none;"
                       "  padding: 1px 10px; font-size: 10px;"
                       "  border-right: 1px solid %3; }"
                       "QToolButton:first { border-top-left-radius: 8px;"
                       "  border-bottom-left-radius: 8px; }"
                       "QToolButton:last { border-right: none;"
                       "  border-top-right-radius: 8px;"
                       "  border-bottom-right-radius: 8px; }"
                       "QToolButton:checked { background: %4; color: %5; }")
                .arg(css(t.surface_raised), css(t.ink_muted), css(t.border_soft),
                     css(t.surface_highest), css(t.ink));
        };
        apply_theme_style(curve_btn, seg_style);
        apply_theme_style(bands_btn, seg_style);
        seg_lay->addWidget(curve_btn);
        seg_lay->addWidget(bands_btn);
        auto* view_lbl = new QLabel(tr("View"), view_row);
        apply_theme_style(view_lbl, [] {
            return QStringLiteral("color: %1; font-size: 10px;")
                .arg(css(tokens().ink_muted));
        });
        view_lay->addWidget(view_lbl);
        view_lay->addWidget(seg);
        view_lay->addStretch(1);
        ac.eq_cat->body_layout()->addWidget(view_row);
    }

    ac.eq_graph = new EqGraphWidget(host);
    ac.eq_cat->body_layout()->addWidget(ac.eq_graph);

    // Band rows: [dot] B1..B6 | type | freq | gain | Q. All six columns are
    // always shown — never folded away per filter type — so the row layout is
    // stable and no label/value is ever hidden. The leading dot is the per-band
    // bypass toggle (EqBand.enabled): the same rule the graph's double-click
    // uses. The band label picks up the band hue while its node is selected,
    // mirroring the graph's selection ring.
    for (int i = 0; i < canvas::core::audio_processing::kEqBandCount; ++i) {
        auto* row = new QWidget(host);
        auto* lay = new QHBoxLayout(row);
        lay->setContentsMargins(0, 0, 0, 0);
        lay->setSpacing(8);
        auto* dot = new QToolButton(row);
        dot->setCheckable(true);
        dot->setChecked(true);
        dot->setFixedSize(14, 14);
        apply_theme_style(dot, [i] {
            const QColor hue = EqGraphWidget::band_hue(i);
            const ThemeTokens& t = tokens();
            return QStringLiteral(
                       "QToolButton { border-radius: 7px; border: 1px solid %1;"
                       "  background-color: %2; }"
                       "QToolButton:checked { background-color: %3; border: 1px solid %3; }")
                .arg(css(with_alpha(hue, 120)), css(t.surface_low), css(hue));
        });
        auto* lbl = new QLabel(QStringLiteral("B%1").arg(i + 1), row);
        lbl->setFixedWidth(22);
        apply_theme_style(lbl, [] {
            return QStringLiteral("color: %1; font-size: 10px;")
                .arg(css(tokens().ink_muted));
        });
        auto* type = make_dark_combo(row);
        type->addItems({tr("Low Shelf"), tr("Bell"), tr("High Shelf"), tr("Low Pass"),
                        tr("High Pass"), tr("Notch")});
        auto* freq = make_band_spin(canvas::core::audio_processing::kEqFreqMin,
                                    canvas::core::audio_processing::kEqFreqMax, 0, 1000.0, row, 78);
        freq->setSuffix(QStringLiteral("Hz"));
        auto* gain = make_band_spin(canvas::core::audio_processing::kEqGainMin,
                                    canvas::core::audio_processing::kEqGainMax, 1, 0.0, row, 66);
        gain->setSuffix(QStringLiteral("dB"));
        auto* q = make_band_spin(canvas::core::audio_processing::kEqQMin,
                                 canvas::core::audio_processing::kEqQMax, 1, 1.0, row, 54);
        lay->addWidget(dot);
        lay->addWidget(lbl);
        lay->addWidget(type, 1);
        lay->addWidget(freq);
        lay->addWidget(gain);
        lay->addWidget(q);

        ac.eq_enable.push_back(dot);
        ac.eq_labels.push_back(lbl);
        ac.eq_type.push_back(type);
        ac.eq_freq.push_back(freq);
        ac.eq_gain.push_back(gain);
        ac.eq_q.push_back(q);
        ac.eq_cat->body_layout()->addWidget(row);
    }

    // --- AI Voice Isolation --------------------------------------------------
    // Real per-clip engine picker (model-backed, applied before the gains/mix
    // in playback AND export). A dropdown row = the engine list; "None" is the
    // default/off state. Modes whose backend isn't compiled in this build stay
    // listed but greyed (DeepFilterNet voice-from-music — seam reserved).
    ac.iso_cat = new InspectorCategory(tr("AI Voice Isolation"), true, /*has_enable=*/false, host);
    {
        auto* row = new QWidget(host);
        auto* lay = new QHBoxLayout(row);
        lay->setContentsMargins(0, 0, 0, 0);
        lay->setSpacing(6);
        auto* combo = make_dark_combo(row);
        // Index i == VoiceIsolationMode i (None=0, RnNoise=1, DeepFilterNet=2);
        // keep this aligned with the enum order.
        combo->addItem(tr("None"));
        combo->addItem(tr("RNNoise — Noise Suppression"));
        combo->addItem(tr("DeepFilterNet — Voice from Music"));
        combo->setItemData(2, tr("Engine not built into this build"),
                           Qt::ToolTipRole);
        if (!canvas::core::voice_isolation_supported(
                canvas::core::VoiceIsolationMode::DeepFilterNet)) {
            if (auto* model_ = qobject_cast<QStandardItemModel*>(combo->model())) {
                if (QStandardItem* item = model_->item(2)) {
                    item->setEnabled(false);
                    item->setToolTip(MainWindow::tr(
                        "DeepFilterNet is not built into this build — RNNoise is the "
                        "shipped engine."));
                }
            }
        }
        lay->addWidget(combo, 1);
        ac.iso_combo = combo;
        add_property_row(ac.iso_cat->body_layout(), tr("Isolate"), row);
        QObject::connect(combo, qOverload<int>(&QComboBox::currentIndexChanged), &mw,
                         [&mw]() { apply_inspector_voice_isolation(mw); });
    }
    audio_layout->addWidget(ac.iso_cat);

    // --- AI sections (UI placeholders, not wired to the model) ---------------
    const auto make_ai = [&](const QString& title, bool with_amount) -> InspectorCategory* {
        auto* cat = new InspectorCategory(title, true, /*has_enable=*/true, host);
        cat->set_feature_toggle_enabled(false);
        cat->set_feature_enabled(false);
        if (with_amount) {
            auto* row = new QWidget(host);
            auto* lay = new QHBoxLayout(row);
            lay->setContentsMargins(0, 0, 0, 0);
            lay->setSpacing(6);
            auto* slider = new QSlider(Qt::Horizontal, row);
            slider->setRange(0, 100);
            slider->setValue(100);
            slider->setEnabled(false);
            slider->setMinimumWidth(0);
            auto* spin = make_numeric(0.0, 100.0, 100.0, row);
            spin->setDecimals(0);
            spin->setEnabled(false);
            lay->addWidget(slider, 1);
            lay->addWidget(spin);
            auto* settings = new QToolButton(row);
            settings->setIcon(icon("settings"));
            settings->setIconSize(QSize(14, 14));
            settings->setAutoRaise(true);
            settings->setToolTip(MainWindow::tr("Additional settings"));
            lay->addWidget(settings);
            add_property_row(cat->body_layout(), tr("Amount"), row);
        }
        audio_layout->addWidget(cat);
        return cat;
    };
    ac.ai_leveler = make_ai(tr("AI Dialogue Leveler"), /*with_amount=*/false);
    ac.ai_remix = make_ai(tr("AI Music Remixer"), /*with_amount=*/false);

    // Equalizer sits at the bottom of the audio tab where it has room to
    // breathe; its extra spacing keeps every band value fully visible.
    audio_layout->addSpacing(8);
    audio_layout->addWidget(ac.eq_cat);

    if (audio_mode_button) {
        audio_mode_button->setToolTip(MainWindow::tr(
            "Audio clip settings — select an audio clip (or a video clip with linked audio)\n"
            "to edit volume, pitch, speed and EQ."));
    }

    // ---- Wiring --------------------------------------------------------------
    QObject::connect(ac.volume, &QDoubleSpinBox::editingFinished, &mw, [&mw]() {
        mw.apply_inspector_audio();
    });
    QObject::connect(ac.pan, &QDoubleSpinBox::editingFinished, &mw, [&mw]() {
        mw.apply_inspector_audio();
    });

    const auto commit_processing = [&mw]() { apply_inspector_audio_processing(mw); };
    for (QDoubleSpinBox* spin : {ac.pitch_semi, ac.pitch_cents, ac.speed_factor})
        QObject::connect(spin, &QDoubleSpinBox::editingFinished, &mw, commit_processing);
    for (QSlider* slider : {ac.pitch_semi_slider, ac.pitch_cents_slider, ac.speed_slider})
        QObject::connect(slider, &QSlider::sliderReleased, &mw, commit_processing);
    for (QDoubleSpinBox* spin : ac.eq_freq)
        QObject::connect(spin, &QDoubleSpinBox::editingFinished, &mw, commit_processing);
    for (QDoubleSpinBox* spin : ac.eq_gain)
        QObject::connect(spin, &QDoubleSpinBox::editingFinished, &mw, commit_processing);
    for (QDoubleSpinBox* spin : ac.eq_q)
        QObject::connect(spin, &QDoubleSpinBox::editingFinished, &mw, commit_processing);
    for (QComboBox* combo : ac.eq_type)
        QObject::connect(combo, qOverload<int>(&QComboBox::currentIndexChanged), &mw, commit_processing);
    for (QToolButton* dot : ac.eq_enable)
        QObject::connect(dot, &QToolButton::toggled, &mw, commit_processing);
    QObject::connect(ac.speed_cat, &InspectorCategory::feature_toggled, &mw, commit_processing);
    QObject::connect(ac.eq_cat, &InspectorCategory::feature_toggled, &mw, commit_processing);

    // Live graph <-> rows echo (no commit): any spin/type/dot edit repaints the
    // curve from the widgets, preserving each band's enabled dot.
    const auto refresh_graph = [&ac]() {
        if (!ac.eq_graph) return;
        std::array<canvas::core::Clip::EqBand, 6> bands;
        for (int i = 0; i < 6 && static_cast<std::size_t>(i) < ac.eq_freq.size(); ++i) {
            bands[i].frequency = ac.eq_freq[i]->value();
            bands[i].gain = ac.eq_gain[i]->value();
            bands[i].q = i < static_cast<int>(ac.eq_q.size()) ? ac.eq_q[i]->value() : 1.0;
            bands[i].type = i < static_cast<int>(ac.eq_type.size())
                                ? static_cast<canvas::core::Clip::EqBand::Type>(
                                      ac.eq_type[i]->currentIndex())
                                : canvas::core::Clip::EqBand::Type::Bell;
            bands[i].enabled = i < static_cast<int>(ac.eq_enable.size()) &&
                               ac.eq_enable[i] && ac.eq_enable[i]->isChecked();
        }
        ac.eq_graph->set_bands(bands);
    };
    for (QDoubleSpinBox* spin : ac.eq_freq)
        QObject::connect(spin, &QDoubleSpinBox::valueChanged, &mw, refresh_graph);
    for (QDoubleSpinBox* spin : ac.eq_gain)
        QObject::connect(spin, &QDoubleSpinBox::valueChanged, &mw, refresh_graph);
    for (QDoubleSpinBox* spin : ac.eq_q)
        QObject::connect(spin, &QDoubleSpinBox::valueChanged, &mw, refresh_graph);
    for (QComboBox* combo : ac.eq_type)
        QObject::connect(combo, qOverload<int>(&QComboBox::currentIndexChanged), &mw, refresh_graph);
    // View toggle: Curve ↔ EasyEffects fader columns. The graph paints from
    // whatever mode is selected; selection is shared across both views.
    if (ac.eq_view_group) {
        QObject::connect(
            ac.eq_view_group, qOverload<int>(&QButtonGroup::idClicked), &mw,
            [&ac](int id) {
                if (!ac.eq_graph) return;
                ac.eq_graph->set_view(id == 1 ? EqGraphWidget::View::Bands
                                              : EqGraphWidget::View::Curve);
            });
    }
    // LP/HP bands carry no gain: grey the row spin as the graph disables its
    // vertical drag, and keep it in lock-step with a type change.
    const auto refresh_band_gain_editable = [&ac]() {
        for (std::size_t i = 0; i < ac.eq_type.size() && i < ac.eq_gain.size(); ++i) {
            if (!ac.eq_type[i] || !ac.eq_gain[i]) continue;
            const auto ty = static_cast<canvas::core::Clip::EqBand::Type>(
                ac.eq_type[i]->currentIndex());
            ac.eq_gain[i]->setEnabled(ty != canvas::core::Clip::EqBand::Type::LowPass &&
                                      ty != canvas::core::Clip::EqBand::Type::HighPass);
        }
    };
    for (QComboBox* combo : ac.eq_type)
        QObject::connect(combo, qOverload<int>(&QComboBox::currentIndexChanged), &mw, refresh_band_gain_editable);

    // Graph gestures → live row echo + one settled commit.
    ac.eq_graph->on_edit = [&ac](int idx) {
        if (idx < 0 || idx >= static_cast<int>(ac.eq_freq.size())) return;
        const auto& b = ac.eq_graph->bands()[idx];
        ac.eq_freq[idx]->setValue(b.frequency);
        ac.eq_gain[idx]->setValue(b.gain);
        ac.eq_q[idx]->setValue(b.q);
        ac.eq_enable[idx]->setChecked(b.enabled);
    };
    ac.eq_graph->on_commit = [&mw]() { apply_inspector_audio_processing(mw); };
    ac.eq_graph->on_selection_changed = [&ac](int idx) {
        // The numeric row echoes the selected node: the B-label takes the band's
        // hue + weight, the others fall back to muted ink.
        for (std::size_t i = 0; i < ac.eq_labels.size(); ++i) {
            const bool sel = static_cast<int>(i) == idx;
            apply_theme_style(ac.eq_labels[i], [sel, i] {
                if (sel) {
                    return QStringLiteral("color: %1; font-size: 10px; font-weight: 600;")
                        .arg(css(EqGraphWidget::band_hue(i)));
                }
                return QStringLiteral("color: %1; font-size: 10px;")
                    .arg(css(tokens().ink_muted));
            });
        }
    };
    // A cell click (or Escape) on the row widgets re-syncs the graph; the
    // selection ring itself is drawn by the graph and echoed here above.
    ac.eq_graph->set_selected(-1);  // harmless no-op initial state
}

void attach_inspector_audio(MainWindow& mw, TimelineWidget* timeline) {
    AudioControls* ac = audio_lookup(mw);
    if (!ac || !ac->volume || !timeline || ac->attached) return;
    ac->attached = true;
    QObject::connect(timeline, &TimelineWidget::clip_selected, &mw,
                     [&mw](const canvas::core::Clip*) { update_inspector_audio_full(mw); });
    QObject::connect(timeline, &TimelineWidget::clips_range_selected, &mw,
                     [&mw](std::vector<canvas::core::ClipId>) { update_inspector_audio_full(mw); });
}

void update_inspector_audio_full(MainWindow& mw) {
    AudioControls* ac = audio_lookup(mw);
    if (!ac || !ac->volume || !mw.project_) return;

    const auto targets = resolve_audio_targets(mw.project_->sequence, mw.selected_clip_ids_);
    const bool has_audio = !targets.empty();

    if (has_audio) populate_from_clip(*ac, targets.front().clip);

    // Enabled for audio clips and for video clips with a linked audio mate.
    if (ac->mode_button) ac->mode_button->setEnabled(has_audio);
    set_processing_enabled(*ac, has_audio);
    if (ac->volume) ac->volume->setEnabled(has_audio);
    if (ac->volume_slider) ac->volume_slider->setEnabled(has_audio);
    if (ac->pan) ac->pan->setEnabled(has_audio);

    // Multi-selection hint: values shown are the FIRST target's, but Volume/Pan
    // commits land on every resolved audio clip.
    if (ac->multi_hint) {
        const int n = static_cast<int>(targets.size());
        ac->multi_hint->setVisible(n > 1);
        if (n > 1)
            ac->multi_hint->setText(
                MainWindow::tr("%1 clips selected — Volume/Pan apply to all").arg(n));
    }
}

void apply_inspector_audio_processing(MainWindow& mw) {
    AudioControls* ac = audio_lookup(mw);
    if (!ac || ac->updating) return;

    canvas::core::Track::Kind kind;
    std::size_t index;
    canvas::core::Clip clip;
    // Target the selected audio clip, or the linked audio mate of a video clip.
    if (!mw.find_audio_target(kind, index, clip)) return;

    const float semi = static_cast<float>(ac->pitch_semi ? ac->pitch_semi->value() : 0.0);
    const float cents = static_cast<float>(ac->pitch_cents ? ac->pitch_cents->value() : 0.0);
    const float speed = static_cast<float>(ac->speed_factor ? ac->speed_factor->value() : 1.0);
    const bool speed_on = ac->speed_cat ? ac->speed_cat->feature_enabled() : false;
    const bool eq_on = ac->eq_cat ? ac->eq_cat->feature_enabled() : false;

    std::array<canvas::core::Clip::EqBand, 6> bands{};
    for (int i = 0; i < 6; ++i) {
        const bool has = i < static_cast<int>(ac->eq_type.size()) &&
                         i < static_cast<int>(ac->eq_freq.size()) &&
                         i < static_cast<int>(ac->eq_gain.size()) &&
                         i < static_cast<int>(ac->eq_q.size());
        bands[i].type = has ? static_cast<canvas::core::Clip::EqBand::Type>(
                                  ac->eq_type[i]->currentIndex())
                            : canvas::core::Clip::EqBand::Type::Bell;
        bands[i].frequency = has ? static_cast<float>(ac->eq_freq[i]->value()) : 1000.0f;
        bands[i].gain = has ? static_cast<float>(ac->eq_gain[i]->value()) : 0.0f;
        bands[i].q = has ? static_cast<float>(ac->eq_q[i]->value()) : 1.0f;
        bands[i].enabled = i < static_cast<int>(ac->eq_enable.size()) && ac->eq_enable[i] &&
                           ac->eq_enable[i]->isChecked();
    }

    const bool same = clip.pitch_semitones == semi && clip.pitch_cents == cents &&
                      clip.speed_factor == speed && clip.speed_enabled == speed_on &&
                      clip.eq_enabled == eq_on && clip.eq_bands == bands;
    if (same) return;

    auto cmd = canvas::core::set_clip_audio_processing(
        mw.project_->sequence, kind, index, clip.id, semi, cents, speed, speed_on, eq_on, bands);
    if (!cmd) return;
    mw.undo_.record(std::move(cmd));
    mw.has_unsaved_changes_ = true;
    mw.refresh_timeline();
    mw.push_audio_mix_snapshot();
    qWarning() << "[edit] CLIP-AUDIO-PROCESSING kind="
               << (kind == canvas::core::Track::Kind::Video ? "V" : "A")
               << "track=" << index << "clip=" << clip.id << "semi=" << semi << "cents=" << cents
               << "speed=" << speed << "eq_on=" << eq_on;
}

void apply_inspector_voice_isolation(MainWindow& mw) {
    AudioControls* ac = audio_lookup(mw);
    if (!ac || ac->updating || !ac->iso_combo) return;

    canvas::core::Track::Kind kind;
    std::size_t index;
    canvas::core::Clip clip;
    // Target the selected audio clip, or the linked audio mate of a video clip.
    if (!mw.find_audio_target(kind, index, clip)) return;

    const auto mode = static_cast<canvas::core::VoiceIsolationMode>(
        ac->iso_combo->currentIndex());
    // An unsupported engine (DeepFilterNet) must never be selectable at commit
    // time even if the entry was force-enabled somehow.
    if (!canvas::core::voice_isolation_supported(mode)) return;
    if (clip.voice_isolation == mode) return;

    auto cmd = canvas::core::set_clip_voice_isolation(mw.project_->sequence, kind, index,
                                                      clip.id, mode);
    if (!cmd) return;
    mw.undo_.record(std::move(cmd));
    mw.has_unsaved_changes_ = true;
    mw.refresh_timeline();
    mw.push_audio_mix_snapshot();
    qWarning() << "[edit] CLIP-VOICE-ISOLATION kind="
               << (kind == canvas::core::Track::Kind::Video ? "V" : "A")
               << "track=" << index << "clip=" << clip.id
               << "mode=" << canvas::core::voice_isolation_mode_name(mode);
}

}  // namespace canvas::gui