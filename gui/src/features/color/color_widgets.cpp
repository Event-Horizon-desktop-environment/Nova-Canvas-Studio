#include "features/color/color_widgets.hpp"
#include <cmath>
#include <utility>
#include <QAbstractSpinBox>
#include <QComboBox>
#include <QDebug>
#include <QDoubleSpinBox>
#include <QFontMetrics>
#include <QGridLayout>
#include <QLineF>
#include <QHBoxLayout>
#include <QLabel>
#include <QLinearGradient>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QResizeEvent>
#include <QShowEvent>
#include <QSlider>
#include <QStackedWidget>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>
#include <array>
#include <cmath>
#include <functional>
#include <utility>

#include "UX/theme.hpp"
#include "features/color/scopes/chromaticity/chromaticity_widget.hpp"
#include "features/color/scopes/histogram/histogram_scope.hpp"
#include "features/color/scopes/parade/parade_scope.hpp"
#include "features/color/scopes/vectorscope/vectorscope_scope.hpp"
#include "features/color/scopes/waveform/waveform_scope.hpp"

namespace canvas::gui {

using namespace canvas::core::colorsci;

// A puck released within this normalized radius of the disc center ([-1,1]^2)
// is a "return-to-center" gesture: the wheel keeps its last committed grade
// rather than committing the neutral puck values. ~5% of the disc radius, in
// line with the small center ring drawn over the wheel face.
constexpr double kCenterReleaseRadius = 0.05;

// ── ToneField ────────────────────────────────────────────────────────────────
// The parameter rows across the panel are drag-scrub fields, not slider
// tracks: click on the box and drag vertically to change the value. A small
// drag threshold keeps a plain click available for keyboard editing.

namespace {
// ScrubSpinBox: a QDoubleSpinBox that scrubs on horizontal drag. Drag distance
// (px) maps to value via `units_per_px`; press stores the anchor, so repeated
// drags accumulate from the value at press time rather than the mid-drag
// value. A click without movement passes through to normal editing.
class ScrubSpinBox : public QDoubleSpinBox {
public:
    explicit ScrubSpinBox(QWidget* parent = nullptr) : QDoubleSpinBox(parent) {
        setButtonSymbols(QAbstractSpinBox::NoButtons);
        setAlignment(Qt::AlignCenter);
        setCursor(Qt::SizeHorCursor);
        setMouseTracking(true);
    }

    void set_units_per_px(double units) { units_per_px_ = units; }
    void set_field_range(double lo, double hi) { field_lo_ = lo; field_hi_ = hi; }

protected:
    void mousePressEvent(QMouseEvent* event) override {
        if (event->button() == Qt::LeftButton) {
            press_x_ = event->position().x();
            press_value_ = value();
            dragging_ = true;
            setFocus(Qt::MouseFocusReason);
        }
        QDoubleSpinBox::mousePressEvent(event);
    }

    void mouseMoveEvent(QMouseEvent* event) override {
        if (dragging_ && (event->buttons() & Qt::LeftButton)) {
            const double dx = event->position().x() - press_x_;
            if (dx != 0.0) {
                setValue(std::clamp(press_value_ + dx * units_per_px_,
                                    field_lo_, field_hi_));
                selectAll();
            }
            event->accept();
            return;
        }
        QDoubleSpinBox::mouseMoveEvent(event);
    }

    void mouseReleaseEvent(QMouseEvent* event) override {
        dragging_ = false;
        QDoubleSpinBox::mouseReleaseEvent(event);
    }

private:
    double units_per_px_ = 0.01;
    double field_lo_ = 0.0;
    double field_hi_ = 1.0;
    double press_value_ = 0.0;
    double press_x_ = 0.0;
    bool dragging_ = false;
};
}  // namespace

// SwatchStrip (at canvas::gui scope so it completes the header forward
// declaration): the mockup's "swatch-slot" — a thin 4px gradient strip under a
// parameter field's box with a small non-interactive marker at the current
// value. kNone renders a transparent slot so every box on a shared row keeps
// the same baseline.
class SwatchStrip : public QWidget {
public:
    explicit SwatchStrip(SwatchKind kind, QWidget* parent = nullptr)
        : QWidget(parent), kind_(kind) {
        setFixedHeight(4);
    }

    void set_marker01(double t01) {
        marker01_ = std::clamp(t01, 0.0, 1.0);
        update();
    }

protected:
    void paintEvent(QPaintEvent* event) override {
        Q_UNUSED(event);
        if (kind_ == SwatchKind::kNone || width() < 8) return;
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        const QRectF r = rect().adjusted(1, 0, -1, 0);

        QLinearGradient g(r.left(), 0.0, r.right(), 0.0);
        switch (kind_) {
            case SwatchKind::kTemp:
                g.setColorAt(0.0, QColor("#4a9ee8"));
                g.setColorAt(0.5, QColor("#c9c9c9"));
                g.setColorAt(1.0, QColor("#e8c34a"));
                break;
            case SwatchKind::kTint:
                g.setColorAt(0.0, QColor("#4ae86e"));
                g.setColorAt(0.5, QColor("#c9c9c9"));
                g.setColorAt(1.0, QColor("#e84ad0"));
                break;
            case SwatchKind::kHue:
                g.setColorAt(0.00, QColor("#4fa2e0"));
                g.setColorAt(0.25, QColor("#4fe070"));
                g.setColorAt(0.50, QColor("#e8d84f"));
                g.setColorAt(0.75, QColor("#e0524f"));
                g.setColorAt(1.00, QColor("#7a6ae8"));
                break;
            case SwatchKind::kNone:
                return;
        }
        p.setPen(Qt::NoPen);
        p.setBrush(g);
        p.drawRoundedRect(r, 2, 2);

        const qreal x = r.left() + marker01_ * r.width();
        p.setPen(QPen(Qt::black, 1.0));
        p.setBrush(QColor("#ffffff"));
        p.drawEllipse(QPointF(x, r.center().y()), 3.5, 3.5);
    }

private:
    SwatchKind kind_ = SwatchKind::kNone;
    double marker01_ = 0.5;
};

ToneField::ToneField(const QString& label, double lo, double hi, double value,
                     double reset_value, QWidget* parent, ToneFieldMode mode,
                     SwatchKind swatch)
    : QWidget(parent), lo_(lo), hi_(hi), reset_value_(reset_value), mode_(mode) {
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(2);

    label_ = new QLabel(label, this);
    apply_theme_style(label_, [] {
        return QStringLiteral("color: %1; font-size: 11px;")
            .arg(css(tokens().ink_muted));
    });
    label_->setAlignment(Qt::AlignHCenter | Qt::AlignVCenter);
    const int box_width = mode == ToneFieldMode::kFieldOnly ? 88 : 58;
    label_->setFixedWidth(box_width);
    root->addWidget(label_, 0, Qt::AlignLeft);

    auto* row = new QHBoxLayout;
    row->setContentsMargins(0, 0, 0, 0);
    row->setSpacing(4);

    auto* scrub = new ScrubSpinBox(this);
    scrub->setRange(lo, hi);
    scrub->setValue(value);
    scrub->setDecimals(2);
    scrub->setFixedWidth(box_width);
    // Full range over a ~200px drag.
    scrub->set_units_per_px((hi - lo) / 200.0);
    scrub->set_field_range(lo, hi);
    apply_theme_style(scrub, [] {
        const ThemeTokens& t = tokens();
        return QStringLiteral(
            "QDoubleSpinBox { background-color: %1; border: 1px solid %2;"
            " border-radius: 6px; padding: 2px 4px; color: %3; font-size: 11px;"
            " selection-background-color: %4; }"
            "QDoubleSpinBox:focus { border-color: %5; }")
            .arg(css(t.surface_low), css(t.border), css(t.ink), css(t.accent),
                 css(t.accent));
    });
    spin_ = scrub;
    row->addWidget(spin_);

    if (mode == ToneFieldMode::kFull || mode == ToneFieldMode::kFieldReset) {
        reset_ = new QToolButton(this);
        reset_->setIcon(icon("reset"));
        reset_->setIconSize(QSize(13, 13));
        reset_->setAutoRaise(true);
        reset_->setFixedWidth(18);
        reset_->setToolTip(tr("Reset to default"));
        apply_theme_style(reset_, &flat_tool_style);
        QObject::connect(reset_, &QToolButton::clicked, this, [this] {
            set_value(reset_value_);
            emit reset_clicked();
        });
        row->addWidget(reset_);
    }

    if (mode == ToneFieldMode::kFull) {
        slider_ = new QSlider(Qt::Horizontal, this);
        slider_->setRange(0, 1000);
        slider_->setValue(static_cast<int>(std::lround(
            (value - lo) / (hi - lo) * 1000.0)));
        slider_->setFixedHeight(16);
        apply_theme_style(slider_, &slider_style);
        row->addWidget(slider_, 1);
    } else {
        row->addStretch(1);
    }
    root->addLayout(row);

    swatch_ = new SwatchStrip(swatch, this);
    swatch_->setFixedWidth(box_width);
    swatch_->set_marker01((value - lo) / (hi - lo));
    root->addWidget(swatch_, 0, Qt::AlignLeft);

    QObject::connect(spin_, qOverload<double>(&QDoubleSpinBox::valueChanged),
                     this, &ToneField::spin_changed);
    if (slider_) {
        QObject::connect(slider_, &QSlider::valueChanged, this,
                         &ToneField::slider_moved);
    }
}

void ToneField::log_geometry(const char* tag) {
    if (label_ == nullptr || spin_ == nullptr || swatch_ == nullptr) return;

    const auto field = mapToParent(rect().topLeft());
    const auto label_pos = label_->mapTo(this, QPoint(0, 0));
    const auto spin_pos = spin_->mapTo(this, QPoint(0, 0));
    const auto swatch_pos = swatch_->mapTo(this, QPoint(0, 0));
    const auto reset_pos = reset_ ? reset_->mapTo(this, QPoint(0, 0))
                                  : QPoint(-1, -1);

    qWarning().nospace()
        << "[tonefield:" << tag << "] label=" << label_->text()
        << " field=" << field.x() << ","
        << width() << "x" << height() << " label_geom=" << label_pos.x() << ","
        << label_pos.y() << "," << label_->width() << "x" << label_->height()
        << " spin_geom=" << spin_pos.x() << "," << spin_pos.y() << ","
        << spin_->width() << "x" << spin_->height()
        << " swatch_geom=" << swatch_pos.x() << "," << swatch_pos.y() << ","
        << swatch_->width() << "x" << swatch_->height()
        << " reset_geom=" << reset_pos.x() << "," << reset_pos.y() << ","
        << (reset_ ? reset_->width() : 0) << "x" << (reset_ ? reset_->height() : 0);
}

void ToneField::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    log_geometry("resize");
}

void ToneField::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);
    log_geometry("show");
}

double ToneField::value() const {
    return spin_ ? spin_->value() : lo_;
}

QString ToneField::label_text() const {
    return label_ ? label_->text() : QString();
}

void ToneField::set_value(double value) {
    if (!spin_) return;
    syncing_ = true;
    spin_->setValue(std::clamp(value, lo_, hi_));
    if (slider_) {
        slider_->setValue(static_cast<int>(std::lround(
            (spin_->value() - lo_) / (hi_ - lo_) * 1000.0)));
    }
    syncing_ = false;
    update_swatch();
}

void ToneField::slider_moved(int pos) {
    if (syncing_ || !spin_ || !slider_) return;
    syncing_ = true;
    const double v = lo_ + (hi_ - lo_) * pos / 1000.0;
    spin_->setValue(v);
    syncing_ = false;
    emit value_changed(v);
    update_swatch();
}

void ToneField::spin_changed(double value) {
    sync_slider_from_spin();
    update_swatch();
    if (!syncing_) emit value_changed(value);
}

void ToneField::update_swatch() {
    if (!swatch_ || !spin_) return;
    swatch_->set_marker01((spin_->value() - lo_) / (hi_ - lo_));
}

void ToneField::sync_slider_from_spin() {
    if (!slider_ || !spin_) return;
    syncing_ = true;
    slider_->setValue(static_cast<int>(std::lround(
        (spin_->value() - lo_) / (hi_ - lo_) * 1000.0)));
    syncing_ = false;
}

// ── MiniKnob ────────────────────────────────────────────────────────────
// A small rotary dial in the style of an old cassette-player volume wheel.
// Grab with the mouse and roll: vertical drag up = increase, down = decrease
// (horizontal movement is ignored so a wheel drag never fights the panel).

MiniKnob::MiniKnob(QWidget* parent) : QWidget(parent) {
    setCursor(Qt::PointingHandCursor);
    setMinimumSize(24, 24);
}

void MiniKnob::set_value01(float t01) {
    t01_ = std::clamp(t01, 0.0f, 1.0f);
    update();
}

void MiniKnob::paintEvent(QPaintEvent* event) {
    Q_UNUSED(event);
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const ThemeTokens& t = tokens();
    const QPointF c = rect().center();
    const qreal r = std::min(width(), height()) / 2.0 - 2.0;

    // Body: machined wheel (radial gradient + ridge ring).
    QRadialGradient body(c, r, c, 0.4 * r);
    body.setColorAt(0.0, t.surface_highest);
    body.setColorAt(1.0, t.surface_low);
    p.setPen(QPen(t.border, 1.0));
    p.setBrush(body);
    p.drawEllipse(c, r, r);

    // Ridge lip where a cassette wheel would have its knurling.
    p.setPen(QPen(with_alpha(t.ink, 60), 1.0));
    for (int i = 0; i < 12; ++i) {
        const qreal a = i * 2 * M_PI / 12;
        p.drawLine(c + QPointF(std::cos(a), std::sin(a)) * (r - 4.0),
                   c + QPointF(std::cos(a), std::sin(a)) * r);
    }

    // Indicator: a pointer that sweeps -135°..+135° with value01, plus a
    // center cap so it reads as a volume knob rather than a clock.
    const qreal deg = -135.0 + 270.0 * t01_;
    const qreal a = deg * M_PI / 180.0;
    p.setPen(QPen(t.accent, 2.0, Qt::SolidLine, Qt::RoundCap));
    p.drawLine(c, c + QPointF(std::cos(a), std::sin(a)) * (r - 5.0));
    p.setPen(QPen(t.surface, 1.0));
    p.setBrush(t.ink);
    p.drawEllipse(c, 2.5, 2.5);
}

void MiniKnob::mousePressEvent(QMouseEvent* event) {
    if (event->button() != Qt::LeftButton) return;
    dragging_ = true;
    press_t01_ = t01_;
    press_pos_ = event->position();
}

void MiniKnob::mouseMoveEvent(QMouseEvent* event) {
    if (!dragging_) return;
    const qreal dy = press_pos_.y() - event->position().y();
    // Full range over ~80px of roll.
    set_value01(press_t01_ + static_cast<float>(dy / 80.0));
    emit value_changed(t01_);
    update();
}

void MiniKnob::mouseReleaseEvent(QMouseEvent* event) {
    if (!dragging_ || event->button() != Qt::LeftButton) return;
    dragging_ = false;
    emit value_committed(t01_);
}

// ── ColorWheelWidget ─────────────────────────────────────────────────────────

ColorWheelWidget::ColorWheelWidget(QWidget* parent) : QWidget(parent) {
    setCursor(Qt::PointingHandCursor);
    setMinimumSize(72, 72);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
}

QRectF ColorWheelWidget::disc_rect() const {
    const qreal r = (std::min(width(), height()) - 6.0) / 2.0;
    return QRectF(width() / 2.0 - r, height() / 2.0 - r, 2.0 * r, 2.0 * r);
}

QPointF ColorWheelWidget::pos_to_xy(const QPointF& pos) const {
    const QPointF c = disc_rect().center();
    const qreal r = disc_rect().width() / 2.0;
    QPointF d(pos.x() - c.x(), pos.y() - c.y());
    const qreal len = std::hypot(d.x(), d.y());
    if (r > 0.0) d /= r;             // xy normalized to [-1,1] on the disc
    if (len > r) d *= r / len;       // clamp inside the disc: radius preserved
    return d;
}

QPointF ColorWheelWidget::xy_to_pos(const QPointF& xy) const {
    const QPointF c = disc_rect().center();
    const qreal r = disc_rect().width() / 2.0;
    return c + xy * r;
}

void ColorWheelWidget::set_xy(const QPointF& xy) {
    xy_ = xy;
    update();
}

void ColorWheelWidget::rebuild_face_cache() {
    const QSize px = face_cache_size_;
    const int cx = px.width() / 2;
    const int cy = px.height() / 2;
    const double rad = std::min(cx, cy) - 1.0;
    const double inner = rad * 0.89;
    QImage img(px, QImage::Format_ARGB32_Premultiplied);
    img.fill(Qt::transparent);
    for (int y = 0; y < px.height(); ++y) {
        for (int x = 0; x < px.width(); ++x) {
            const int dx = x - cx;
            const int dy = y - cy;
            const double d = std::hypot(static_cast<double>(dx), static_cast<double>(dy));
            const double soft = 1.5;
            if (d > rad + soft)
                continue;
            const double ang = std::atan2(static_cast<double>(dy), static_cast<double>(dx));
            double h = ang / (2.0 * std::acos(-1.0));
            if (h < 0.0)
                h += 1.0;
            double v = 0.72;
            if (d > inner) {
                const double t = (d - inner) / (rad - inner);
                v = 0.72 + 0.20 * t;
            }
            QColor col = QColor::fromHsvF(h, 1.0, v);
            if (d > rad)
                col.setAlphaF(std::max(0.0, (rad + soft - d) / soft));
            img.setPixelColor(x, y, col);
        }
    }
    face_cache_ = std::move(img);
}

void ColorWheelWidget::paintEvent(QPaintEvent* event) {
    Q_UNUSED(event);
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const ThemeTokens& t = tokens();

    const QRectF disc = disc_rect();
    const qreal r = disc.width() / 2.0;
    const QPointF c = disc.center();

    // Hue wheel: per-pixel HSV bake (angle -> hue) cached in face_cache_ —
    // covers the ENTIRE disc (core + rim band) with every hue around the rim,
    // no white core, no RGB-interpolation banding. The rim band is the same hue
    // map at a higher value so it matches the core colors but stands out.
    const QRectF face = disc.adjusted(r * 0.11, r * 0.11, -r * 0.11, -r * 0.11);
    const QSize disc_px = disc.size().toSize();
    if (disc_px != face_cache_size_) {
        face_cache_size_ = disc_px;
        rebuild_face_cache();
    }
    p.save();
    QPainterPath wheel_clip;
    wheel_clip.addEllipse(disc);
    p.setClipPath(wheel_clip);
    p.setRenderHint(QPainter::SmoothPixmapTransform, true);
    p.drawImage(disc, face_cache_);
    p.restore();

    // Crosshair lines + center ring (mockup wheel-face anatomy).
    p.setPen(QPen(QColor(255, 255, 255, 40), 1.0));
    p.drawLine(QPointF(face.left(), c.y()), QPointF(face.right(), c.y()));
    p.drawLine(QPointF(c.x(), face.top()), QPointF(c.x(), face.bottom()));
    p.setPen(QPen(QColor("#ececec"), 1.5));
    p.setBrush(Qt::NoBrush);
    p.drawEllipse(c, r * 0.055, r * 0.055);

    // Position marker.
    const QPointF dot = xy_to_pos(xy_);
    p.setPen(QPen(active_ ? t.accent : t.ink, 2.0));
    p.setBrush(active_ ? t.accent_soft : QColor(Qt::transparent));
    p.drawEllipse(dot, 5.0, 5.0);
    p.setPen(QPen(t.surface_highest, 1.2));
    p.setBrush(active_ ? t.accent : t.ink);
    p.drawEllipse(dot, 2.0, 2.0);
}

void ColorWheelWidget::mousePressEvent(QMouseEvent* event) {
    if (event->button() != Qt::LeftButton) return;
    const QPointF d = event->position() - disc_rect().center();
    if (d.manhattanLength() > disc_rect().width() / 2.0 + 2.0) return;
    dragging_ = true;
    active_ = true;
    set_xy(pos_to_xy(event->position()));
    update();
}

void ColorWheelWidget::mouseMoveEvent(QMouseEvent* event) {
    if (!dragging_) return;
    set_xy(pos_to_xy(event->position()));
    emit xy_changed(xy_);
    update();
}

void ColorWheelWidget::mouseReleaseEvent(QMouseEvent* event) {
    if (!dragging_ || event->button() != Qt::LeftButton) {
        QWidget::mouseReleaseEvent(event);
        return;
    }
    dragging_ = false;
    set_xy(pos_to_xy(event->position()));
    emit xy_changed(xy_);
    emit xy_committed(xy_);
    update();
}

// ── ColorWheelsPanel ─────────────────────────────────────────────────────────

ColorWheelsPanel::ColorWheelsPanel(QWidget* parent) : QWidget(parent) {
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(10, 10, 10, 10);
    root->setSpacing(8);

    // Header: title (left) + icon-button cluster (right), the shared
    // panel-header contract every Grading-Workspace panel follows — title
    // left · icon cluster right (reset-all, view options, overflow).
    auto* header = new QWidget(this);
    auto* header_l = new QHBoxLayout(header);
    header_l->setContentsMargins(0, 0, 0, 0);
    header_l->setSpacing(2);
    title_ = new QLabel(header);
    title_->setText({});
    apply_theme_style(title_, [] {
        const ThemeTokens& t = tokens();
        return QStringLiteral("color: %1; font-size: 12px; font-weight: 600;")
            .arg(css(t.ink));
    });
    header_l->addWidget(title_, 1);

    auto make_header_icon = [this, header, header_l](const char* icon_name,
                                                       const QString& tooltip,
                                                       const std::function<void()>& on_click) {
        auto* b = new QToolButton(header);
        b->setIcon(icon(icon_name));
        b->setIconSize(QSize(14, 14));
        b->setAutoRaise(true);
        b->setFixedSize(24, 24);
        b->setToolTip(tooltip);
        apply_theme_style(b, &flat_tool_style);
        if (on_click) QObject::connect(b, &QToolButton::clicked, this, on_click);
        header_l->addWidget(b);
        return b;
    };

    // Reset-all: resets the whole panel state to identity; the page owns the
    // single undo so it can also clear the Curves panel (see
    // reset_all_requested in the header) and write a truly empty grade.
    make_header_icon("reset", tr("Reset all grades"), [this] {
        qWarning().nospace() << "[grade] reset-all";
        reset_panel(state_);
        set_state(state_);
        emit reset_all_requested();
    });
    // View options / overflow: structural placeholders to match the panel
    // header contract; no behavior yet.
    make_header_icon("mode", tr("View options"), {});
    make_header_icon("menu", tr("More options"), {});
    root->addWidget(header);

    const auto add_tone = [this](const QString& label, double lo, double hi,
                                 double value, double reset, int param,
                                 QWidget* parent, QHBoxLayout* target,
                                 ToneFieldMode mode = ToneFieldMode::kFieldReset,
                                 SwatchKind swatch = SwatchKind::kNone) {
        auto* t = new ToneField(label, lo, hi, value, reset, parent, mode, swatch);
        tone_fields_.append(t);
        target->addWidget(t, 1);
        QObject::connect(t, &ToneField::value_changed, this,
                         [this, param](double v) { tone_param_changed(param, v); });
        return t;
    };

    // NOTE: no parameter row above the wheels — mockup layout keeps the tone
    // fields in a single shared row BELOW the wheels only.

    // Spec §9a item 2: exactly four circular wheels, evenly spaced, in fixed
    // order Lift, Gamma, Gain, Offset. Each wheel column follows the mockup's
    // `wtop` anatomy: a dial-block on the LEFT (small cassette master knob +
    // its value), the wheel NAME centered, and the reset icon on the right —
    // symmetric fixed-width ends so the name centers on the wheel's axis.
    const char* const kWheelNames[4] = {"Lift", "Gamma", "Gain", "Offset"};
    auto* wheels_row = new QWidget(this);
    auto* wheels_layout = new QHBoxLayout(wheels_row);
    wheels_layout->setContentsMargins(0, 0, 0, 0);
    wheels_layout->setSpacing(12);
    for (int i = 0; i < 4; ++i) {
        auto* cell = new QWidget(wheels_row);
        auto* cell_layout = new QVBoxLayout(cell);
        cell_layout->setContentsMargins(0, 0, 0, 0);
        cell_layout->setSpacing(2);

        // wtop: three-column header — dial-block | name | reset.
        auto* wheel_header = new QWidget(cell);
        auto* wheel_header_l = new QHBoxLayout(wheel_header);
        wheel_header_l->setContentsMargins(0, 0, 0, 0);
        wheel_header_l->setSpacing(0);

        // Dial-block: hidden master state (identity mid by default) + the
        // current master-term readout text. The cassette knob visual is gone;
        // the master stays at its identity mid so the wheel response law keeps
        // working, and the readout text + reset button remain.
        auto* dial_block = new QWidget(wheel_header);
        auto* dial_l = new QVBoxLayout(dial_block);
        dial_l->setContentsMargins(0, 0, 0, 0);
        dial_l->setSpacing(0);
        auto* master = new MiniKnob(dial_block);
        master->setToolTip(tr("Master"));
        master->setFixedSize(20, 20);
        // Start at the law's identity mid (0.5 -> Lift=0, Gamma/Gain=1, Offset=0).
        master->set_value01(0.5f);
        master->hide();
        auto* dial_val = new QLabel("0.00", dial_block);
        dial_val->setAlignment(Qt::AlignHCenter);
        apply_theme_style(dial_val, [] {
            return QStringLiteral("color: %1; font-size: 9px; font-family: monospace;")
                .arg(css(tokens().ink_muted));
        });
        dial_l->addStretch(1);
        dial_l->addWidget(dial_val);
        dial_l->addStretch(1);
        dial_block->setFixedWidth(30);
        wheel_header_l->addWidget(dial_block);

        auto* name = new QLabel(kWheelNames[i], wheel_header);
        apply_theme_style(name, [] {
            return QStringLiteral("color: %1; font-size: 11px; font-weight: 550;")
                .arg(css(tokens().ink_muted));
        });
        name->setAlignment(Qt::AlignCenter);
        wheel_header_l->addWidget(name, 1);
        auto* reset = new QToolButton(wheel_header);
        reset->setIcon(icon("reset"));
        reset->setIconSize(QSize(12, 12));
        reset->setAutoRaise(true);
        reset->setFixedWidth(30);
        reset->setFixedHeight(20);
        reset->setToolTip(tr("Reset wheel"));
        apply_theme_style(reset, &flat_tool_style);
        wheel_header_l->addWidget(reset);
        cell_layout->addWidget(wheel_header);

        auto* wheel = new ColorWheelWidget(cell);
        cell_layout->addWidget(wheel, 1);

        // Three small boxed per-channel numeric readouts (spec item 2).
        auto* channel_row = new QWidget(cell);
        auto* channel_layout = new QHBoxLayout(channel_row);
        channel_layout->setContentsMargins(0, 0, 0, 0);
        channel_layout->setSpacing(2);
        QVector<QLabel*> boxes;
        for (int ch = 0; ch < 3; ++ch) {
            auto* v = new QLabel("0.000", channel_row);
            v->setAlignment(Qt::AlignCenter);
            apply_theme_style(v, [] {
                const ThemeTokens& t = tokens();
                return QStringLiteral(
                    "QLabel { background-color: %1; border: 1px solid %2;"
                    " border-radius: 4px; padding: 1px 2px; color: %3;"
                    " font-size: 10px; font-family: monospace; }")
                    .arg(css(t.surface_low), css(t.border_soft), css(t.ink));
            });
            channel_layout->addWidget(v, 1);
            boxes.append(v);
        }
        channel_readouts_.append(boxes);
        cell_layout->addWidget(channel_row);

        wheels_layout->addWidget(cell, 1);
        wheels_.append(wheel);
        masters_.append(master);
        master_values_.append(dial_val);
        const int idx = i;
        QObject::connect(wheel, &ColorWheelWidget::xy_changed, this,
                         [this, idx](const QPointF& xy) { wheel_moved(idx, xy); });
        QObject::connect(wheel, &ColorWheelWidget::xy_committed, this,
                         [this, idx](const QPointF& xy) { wheel_committed(idx, xy); });
        QObject::connect(master, &MiniKnob::value_changed, this,
                         [this, idx](float t01) { master_moved(idx, t01); });
        QObject::connect(master, &MiniKnob::value_committed, this,
                         [this, idx](float t01) {
                             master_moved(idx, t01);
                             commit();
                         });
        QObject::connect(reset, &QToolButton::clicked, this, [this, idx] {
            qWarning().nospace() << "[grade] wheel-reset idx=" << idx;
            wheels_[idx]->set_xy(QPointF(0.0, 0.0));
            masters_[idx]->set_value01(0.5f);
            // Reset through the controller law so the state matches the widgets.
            reset_primaries_wheel(state_, static_cast<PrimariesWheel>(idx));
            refresh_wheel_readout(idx);
            commit();
        });
    }
    root->addWidget(wheels_row, 1);

    // The ONE shared tone row BELOW the wheels, matching the mockup's shared
    // row exactly: Temp, Tint, Hue, Contrast, Pivot, Mid/Detail, Blk/Offset.
    // Enum order == construction order below, so set_state's enum-int indexing
    // stays valid. Color Boost, Shadows, Highlights, Saturation, Lum Mix are
    // retained in WheelPanelState but no longer surfaced in the panel.
    auto* param_row = new QWidget(this);
    auto* param_layout = new QHBoxLayout(param_row);
    param_layout->setContentsMargins(0, 0, 0, 0);
    param_layout->setSpacing(8);
    add_tone(tr("Temp"), kTempLo, kTempHi, 0.0, 0.0,
             static_cast<int>(ToneParam::kTemp), param_row, param_layout,
             ToneFieldMode::kFieldReset, SwatchKind::kTemp);
    add_tone(tr("Tint"), kTintLo, kTintHi, 0.0, 0.0,
             static_cast<int>(ToneParam::kTint), param_row, param_layout,
             ToneFieldMode::kFieldReset, SwatchKind::kTint);
    add_tone(tr("Hue"), kHueDegLo, kHueDegHi, 0.0, 0.0,
             static_cast<int>(ToneParam::kHue), param_row, param_layout,
             ToneFieldMode::kFieldReset, SwatchKind::kHue);
    add_tone(tr("Contrast"), kContrastLo, kContrastHi, 1.0, 1.0,
             static_cast<int>(ToneParam::kContrast), param_row, param_layout);
    add_tone(tr("Pivot"), kPivotLo, kPivotHi, kDefaultPivot, kDefaultPivot,
             static_cast<int>(ToneParam::kPivot), param_row, param_layout);
    add_tone(tr("Mid/Detail"), kMidDetailLo, kMidDetailHi, 0.0, 0.0,
             static_cast<int>(ToneParam::kMidDetail), param_row, param_layout);
    add_tone(tr("Blk/Offset"), kOffsetLo, kOffsetHi, 0.0, 0.0,
             static_cast<int>(ToneParam::kBlackOffset), param_row, param_layout);
    root->addWidget(param_row);

    // The tone reset button also commits (its value already snapped to the
    // reset value via set_value -> value_changed).
    for (ToneField* t : tone_fields_) {
        QObject::connect(t, &ToneField::reset_clicked, this, [this, t] {
            qWarning().nospace() << "[grade] tone-reset field=" << t->label_text();
            commit();
        });
    }
}

bool ColorWheelsPanel::interaction_log_gate() {
    const auto now = std::chrono::steady_clock::now();
    if (now - last_interaction_log_ < std::chrono::milliseconds(100)) return false;
    last_interaction_log_ = now;
    return true;
}

void ColorWheelsPanel::set_state(const canvas::core::colorsci::WheelPanelState& state) {
    state_ = state;
    // Push the loaded wheels/tone back into the widgets without committing.
    for (int i = 0; i < wheels_.size(); ++i) {
        wheels_[i]->set_xy(QPointF(0.0, 0.0));
        const auto& meta = canvas::core::colorsci::detail::kWheelMeta[i];
        const float master = [&] {
            switch (static_cast<PrimariesWheel>(i)) {
                case PrimariesWheel::kLift: return state_.lgg.lift_master;
                case PrimariesWheel::kGamma: return state_.lgg.gamma_master;
                case PrimariesWheel::kGain: return state_.lgg.gain_master;
                case PrimariesWheel::kOffset: return state_.offset.master;
            }
            return 0.0f;
        }();
        const float t01 = value_to_master(master, meta.lo_master, meta.hi_master, meta.id_master);
        masters_[i]->set_value01(t01);
        refresh_wheel_readout(i);
    }
    tone_fields_[static_cast<int>(ToneParam::kTemp)]->set_value(state_.temp);
    tone_fields_[static_cast<int>(ToneParam::kTint)]->set_value(state_.tint);
    tone_fields_[static_cast<int>(ToneParam::kHue)]->set_value(state_.hue_deg);
    tone_fields_[static_cast<int>(ToneParam::kContrast)]->set_value(state_.contrast);
    tone_fields_[static_cast<int>(ToneParam::kPivot)]->set_value(state_.pivot);
    tone_fields_[static_cast<int>(ToneParam::kMidDetail)]->set_value(state_.mid_detail);
    tone_fields_[static_cast<int>(ToneParam::kBlackOffset)]->set_value(state_.black_offset);
    // Always-on load trace: proves a clip's saved grade actually reached the
    // boards (the wheels come up CENTERED, so this line is the only proof the
    // panel is not showing fresh identity defaults).
    qWarning().nospace()
        << "[grade] wheel-panel load lift_m=" << state_.lgg.lift_master
        << " gamma_m=" << state_.lgg.gamma_master
        << " gain_m=" << state_.lgg.gain_master
        << " offset_m=" << state_.offset.master
        << " lift_r=" << state_.lgg.lift_r
        << " temp=" << state_.temp << " contrast=" << state_.contrast;
}

canvas::core::colorsci::WheelPanelState ColorWheelsPanel::state() const {
    return state_;
}

void ColorWheelsPanel::wheel_moved(int index, const QPointF& xy) {
    const auto wheel = static_cast<PrimariesWheel>(index);
    const float master01 = masters_[index]->value01();
    // Feed the wheel through the controller law. The lift/gamma/gain/offset
    // per-channel terms land in the state and the boxes below the wheel track
    // them exactly.
    apply_primaries_wheel(state_, wheel, xy.x(), xy.y(), master01);
    if (is_center_release(xy)) {
        // Center = revert: a puck moved back onto the disc center reverts this
        // wheel to identity (same law as the per-wheel reset button). Preview
        // matches the commit rule so a center release commits what was shown.
        reset_primaries_wheel(state_, wheel);
        masters_[index]->set_value01(0.5f);
    }
    wheels_[index]->set_active(true);
    refresh_wheel_readout(index);
    if (interaction_log_gate()) {
        const float radius = std::hypot(xy.x(), xy.y());
        const float scale = canvas::core::colorsci::detail::kWheelMeta[index].scale;
        const auto off = wheel_offset_for_roundtrip(index);
        qWarning().nospace()
            << "[grade] wheel-move idx=" << index
            << " xy=(" << QString::number(xy.x(), 'f', 3) << ","
            << QString::number(xy.y(), 'f', 3) << ")"
            << " radius=" << QString::number(radius, 'f', 3)
            << " scale=" << QString::number(scale, 'f', 3)
            << " master=" << QString::number(master01, 'f', 3)
            << " revert=" << (is_center_release(xy) ? 1 : 0)
            << " -> off=(" << QString::number(off[0], 'f', 3) << ","
            << QString::number(off[1], 'f', 3) << ","
            << QString::number(off[2], 'f', 3) << ")";
    }
    emit params_preview();
}

bool ColorWheelsPanel::is_center_release(const QPointF& xy) const {
    return std::hypot(xy.x(), xy.y()) <= kCenterReleaseRadius;
}

// Post-law per-channel offset that a wheel's state currently holds — the exact
// terms the LUT bake consumes (not the puck xy, which the scale law transforms
// before it reaches the state). Printed alongside radius/scale in the
// move/release traces so a "small-feeling drag" can be checked end-to-end:
// radius -> scaled offset -> committed grade, all in one log.
std::array<float, 3> ColorWheelsPanel::wheel_offset_for_roundtrip(int index) const {
    switch (static_cast<PrimariesWheel>(index)) {
        case PrimariesWheel::kLift:
            return {state_.lgg.lift_r, state_.lgg.lift_g, state_.lgg.lift_b};
        case PrimariesWheel::kGamma: {
            const auto& w = state_.lgg;
            return {w.gamma_r - 1.0f, w.gamma_g - 1.0f, w.gamma_b - 1.0f};
        }
        case PrimariesWheel::kGain: {
            const auto& w = state_.lgg;
            return {w.gain_r - 1.0f, w.gain_g - 1.0f, w.gain_b - 1.0f};
        }
        case PrimariesWheel::kOffset: {
            const auto& o = state_.offset;
            return {o.r, o.g, o.b};
        }
    }
    return {0.0f, 0.0f, 0.0f};
}

void ColorWheelsPanel::wheel_committed(int index, const QPointF& xy) {
    const auto wheel = static_cast<PrimariesWheel>(index);
    const float master01 = masters_[index]->value01();
    apply_primaries_wheel(state_, wheel, xy.x(), xy.y(), master01);
    if (is_center_release(xy)) {
        // Return-to-center reverts, not cancels: releasing a puck back on the
        // disc center clears this wheel's grade to identity, same law as the
        // per-wheel reset button. The committed params below carry identity for
        // this wheel (other wheels/tone keep their committed values).
        reset_primaries_wheel(state_, wheel);
        masters_[index]->set_value01(0.5f);
    }
    refresh_wheel_readout(index);
    // Always-on release trace: the xy + revert flag make the return-to-center
    // semantic legible — a center release logs revert=1 and the commit below
    // writes identity for this wheel.
    {
        const float radius = std::hypot(xy.x(), xy.y());
        const float scale = canvas::core::colorsci::detail::kWheelMeta[index].scale;
        const auto off = wheel_offset_for_roundtrip(index);
        qWarning().nospace()
            << "[grade] wheel-release idx=" << index
            << " xy=(" << QString::number(xy.x(), 'f', 3) << ","
            << QString::number(xy.y(), 'f', 3) << ")"
            << " radius=" << QString::number(radius, 'f', 3)
            << " scale=" << QString::number(scale, 'f', 3)
            << " master=" << QString::number(master01, 'f', 3)
            << " revert=" << (is_center_release(xy) ? 1 : 0)
            << " -> off=(" << QString::number(off[0], 'f', 3) << ","
            << QString::number(off[1], 'f', 3) << ","
            << QString::number(off[2], 'f', 3) << ")";
    }
    commit();
}

void ColorWheelsPanel::master_moved(int index, float t01) {
    const QPointF xy = wheels_[index]->xy();
    apply_primaries_wheel(state_, static_cast<PrimariesWheel>(index), xy.x(), xy.y(), t01);
    refresh_wheel_readout(index);
    if (interaction_log_gate()) {
        qWarning().nospace()
            << "[grade] master-move idx=" << index
            << " t01=" << QString::number(t01, 'f', 3);
    }
    emit params_preview();
}

void ColorWheelsPanel::refresh_wheel_readout(int index) {
    if (index < 0 || index >= channel_readouts_.size()) return;
    const auto& meta = canvas::core::colorsci::detail::kWheelMeta[index];
    std::array<float, 3> values{0.0f, 0.0f, 0.0f};
    switch (static_cast<PrimariesWheel>(index)) {
        case PrimariesWheel::kLift:
            values = {state_.lgg.lift_r, state_.lgg.lift_g, state_.lgg.lift_b};
            break;
        case PrimariesWheel::kGamma:
            values = {state_.lgg.gamma_r, state_.lgg.gamma_g, state_.lgg.gamma_b};
            break;
        case PrimariesWheel::kGain:
            values = {state_.lgg.gain_r, state_.lgg.gain_g, state_.lgg.gain_b};
            break;
        case PrimariesWheel::kOffset:
            values = {state_.offset.r, state_.offset.g, state_.offset.b};
            break;
    }
    for (int ch = 0; ch < 3; ++ch) {
        channel_readouts_[index][ch]->setText(QString::number(values[ch], 'f', 3));
    }
    if (index < master_values_.size() && masters_.size() > static_cast<qsizetype>(index)) {
        const float master = master_to_value(
            masters_[index]->value01(), meta.lo_master, meta.hi_master, meta.id_master);
        master_values_[index]->setText(QString::number(master, 'f', 2));
    }
    if (static_cast<PrimariesWheel>(index) == PrimariesWheel::kOffset) {
        // Keep the Blk/Offset field in lock-step with the Offset wheel's master.
        state_.black_offset = state_.offset.master;
        tone_fields_[static_cast<int>(ToneParam::kBlackOffset)]->set_value(state_.black_offset);
    }
}

void ColorWheelsPanel::tone_param_changed(int param, double value) {
    switch (static_cast<ToneParam>(param)) {
        case ToneParam::kTemp: state_.temp = static_cast<float>(value); break;
        case ToneParam::kTint: state_.tint = static_cast<float>(value); break;
        case ToneParam::kHue: state_.hue_deg = static_cast<float>(value); break;
        case ToneParam::kContrast: state_.contrast = static_cast<float>(value); break;
        case ToneParam::kPivot: state_.pivot = static_cast<float>(value); break;
        case ToneParam::kMidDetail: state_.mid_detail = static_cast<float>(value); break;
        case ToneParam::kBlackOffset: {
            // Blk/Offset drives the Offset wheel's master through the same
            // master01->value law as the cassette knob. Sync the knob so the
            // wheel's master term and the field never diverge.
            state_.black_offset = static_cast<float>(value);
            state_.offset.master = static_cast<float>(value);
            const auto& meta = canvas::core::colorsci::detail::kWheelMeta[static_cast<int>(PrimariesWheel::kOffset)];
            masters_[static_cast<int>(PrimariesWheel::kOffset)]->set_value01(
                value_to_master(state_.offset.master, meta.lo_master, meta.hi_master, meta.id_master));
            refresh_wheel_readout(static_cast<int>(PrimariesWheel::kOffset));
            break;
        }
    }
    if (interaction_log_gate()) {
        qWarning().nospace()
            << "[grade] tone-param param=" << param
            << " value=" << QString::number(value, 'f', 2);
    }
    commit();
}

void ColorWheelsPanel::commit() {
    // Gates shared with the interaction taps: tone scrubs commit per value
    // change, so an ungated line here would still flush at mouse-move rate.
    // Each interaction surface (wheel release / tone-param / reset) logs its
    // own explicit line on top of this digest.
    if (interaction_log_gate()) {
        qWarning().nospace()
            << "[grade] wheel-commit lift_m=" << state_.lgg.lift_master
            << " gamma_m=" << state_.lgg.gamma_master
            << " gain_m=" << state_.lgg.gain_master
            << " offset_m=" << state_.offset.master;
    }
    emit params_committed(state_);
}


// ── ScopesPanel ──────────────────────────────────────────────────────────────

ScopesPanel::ScopesPanel(QWidget* parent) : QWidget(parent) {
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(10, 10, 10, 10);
    root->setSpacing(8);

    auto* header = new QWidget(this);
    auto* header_row = new QHBoxLayout(header);
    header_row->setContentsMargins(0, 0, 0, 0);
    header_row->setSpacing(6);

    auto* title = new QLabel(tr("Scopes"), header);
    apply_theme_style(title, [] {
        const ThemeTokens& t = tokens();
        return QStringLiteral("color: %1; font-size: 12px; font-weight: 600;")
            .arg(css(t.ink));
    });
    header_row->addWidget(title, 1);

    auto* mode = new QComboBox(header);
    mode->addItems({tr("Waveform"), tr("Parade"), tr("Vectorscope"),
                    tr("Histogram"), tr("CIE")});
    apply_theme_style(mode, &flat_tool_style);
    header_row->addWidget(mode);

    auto make_toggle = [this](const QIcon& ic, const char* tip) {
        auto* b = new QToolButton(this);
        b->setIcon(ic);
        b->setIconSize(QSize(14, 14));
        b->setToolTip(tr(tip));
        b->setCheckable(true);
        b->setAutoRaise(true);
        apply_theme_style(b, &flat_tool_style);
        return b;
    };
    header_row->addWidget(make_toggle(icon("mark_in", QColor(0xC9, 0x86, 0x3A)), "Split screen"));
    auto* fifty = make_toggle(icon("mark_out", QColor(0xC9, 0x86, 0x3A)), "50% zoom");
    fifty->setChecked(true);
    header_row->addWidget(fifty);
    root->addWidget(header);

    // One page per ScopeMode value (stack index == enum value). Every scope is a
    // real widget fed the presented frame; see scopes/<name>/ for each one.
    waveform_scope_ = new WaveformScope(this);
    parade_scope_ = new ParadeScope(this);
    vectorscope_scope_ = new VectorscopeScope(this);
    histogram_scope_ = new HistogramScope(this);
    chromaticity_ = new ChromaticityWidget(this);
    stack_ = new QStackedWidget(this);
    stack_->addWidget(waveform_scope_);
    stack_->addWidget(parade_scope_);
    stack_->addWidget(vectorscope_scope_);
    stack_->addWidget(histogram_scope_);
    stack_->addWidget(chromaticity_);
    root->addWidget(stack_, 1);

    // Sub-display dropdown: Waveform (Luma/RGB/YRGB), later Histogram
    // (Luma/RGB); hidden for every other mode.
    display_sub_ = new QComboBox(header);
    apply_theme_style(display_sub_, &flat_tool_style);
    display_sub_->hide();
    header_row->addWidget(display_sub_);

    // Vectorscope options (wave spec §2 panel-specific): trace shading mode,
    // gain slider + 2x zoom.
    vector_opts_ = new QWidget(header);
    auto* vopt_row = new QHBoxLayout(vector_opts_);
    vopt_row->setContentsMargins(0, 0, 0, 0);
    vopt_row->setSpacing(4);
    auto* trace_label = new QLabel(tr("Trace"), vector_opts_);
    apply_theme_style(trace_label, [] {
        const ThemeTokens& t = tokens();
        return QStringLiteral("color: %1; font-size: 10px;").arg(css(t.ink));
    });
    vopt_row->addWidget(trace_label);
    vec_trace_ = new QComboBox(vector_opts_);
    vec_trace_->addItems({tr("Color"), tr("Mono"), tr("Green")});
    vec_trace_->setCurrentIndex(int(canvas::gui::TraceMode::Color));
    apply_theme_style(vec_trace_, &flat_tool_style);
    vopt_row->addWidget(vec_trace_);
    auto* sens_label = new QLabel(tr("Sens"), vector_opts_);
    apply_theme_style(sens_label, [] {
        const ThemeTokens& t = tokens();
        return QStringLiteral("color: %1; font-size: 10px;").arg(css(t.ink));
    });
    vopt_row->addWidget(sens_label);
    sens_slider_ = new QSlider(Qt::Horizontal, vector_opts_);
    sens_slider_->setRange(10, 300);
    sens_slider_->setValue(100);
    sens_slider_->setFixedWidth(72);
    apply_theme_style(sens_slider_, &flat_tool_style);
    vopt_row->addWidget(sens_slider_);
    zoom2x_btn_ = make_toggle(icon("zoom_in", QColor(0xC9, 0x86, 0x3A)), "2x zoom");
    vopt_row->addWidget(zoom2x_btn_);
    vector_opts_->hide();
    header_row->addWidget(vector_opts_);

    // Wire the dropdown BEFORE selecting the default mode, so the initial
    // setCurrentIndex below actually flips the stacked widget to the real
    // Parade scope on startup instead of leaving the placeholder on screen.
    QObject::connect(mode, qOverload<int>(&QComboBox::currentIndexChanged),
                     this, [this](int i) { set_mode(static_cast<ScopeMode>(i)); });
    QObject::connect(display_sub_, qOverload<int>(&QComboBox::currentIndexChanged),
                     this, &ScopesPanel::set_sub_display);
    QObject::connect(vec_trace_, qOverload<int>(&QComboBox::currentIndexChanged),
                     this, [this](int i) {
                         if (vectorscope_scope_) {
                             vectorscope_scope_->set_trace_mode(static_cast<TraceMode>(i));
                         }
                     });
    QObject::connect(sens_slider_, &QSlider::valueChanged,
                     vectorscope_scope_, &VectorscopeScope::set_gain_percent);
    QObject::connect(zoom2x_btn_, &QToolButton::toggled,
                     vectorscope_scope_, &VectorscopeScope::set_zoom2x);

    // Parade is the live-fed default so a fresh project shows real signal.
    mode->setCurrentIndex(int(ScopeMode::Parade));
}

void ScopesPanel::set_mode(ScopeMode mode) {
    mode_ = mode;
    stack_->setCurrentIndex(int(mode));

    display_sub_->blockSignals(true);
    display_sub_->clear();
    if (mode == ScopeMode::Waveform) {
        display_sub_->addItems({tr("Luma"), tr("RGB"), tr("YRGB")});
        display_sub_->setCurrentIndex(int(waveform_scope_->display()));
        display_sub_->show();
    } else if (mode == ScopeMode::Histogram) {
        display_sub_->addItems({tr("Luma"), tr("RGB")});
        display_sub_->setCurrentIndex(int(histogram_scope_->display()));
        display_sub_->show();
    } else {
        display_sub_->hide();
    }
    display_sub_->blockSignals(false);

    vector_opts_->setVisible(mode == ScopeMode::Vectorscope);

    // Re-feed the newest presented frame so a reclaimed page shows real signal.
    feed_frame_to_page();
    if (mode == ScopeMode::CIE) chromaticity_->refresh();
}

void ScopesPanel::set_sub_display(int index) {
    if (!display_sub_->isVisible() || index < 0) return;
    if (mode_ == ScopeMode::Waveform) {
        waveform_scope_->set_display(static_cast<ScopeDisplay>(index));
    } else if (mode_ == ScopeMode::Histogram) {
        histogram_scope_->set_display(static_cast<ScopeDisplay>(index));
    }
}

void ScopesPanel::feed_frame_to_page() {
    if (!last_frame_) return;
    switch (mode_) {
        case ScopeMode::Waveform: waveform_scope_->update_frame(last_frame_); break;
        case ScopeMode::Parade: parade_scope_->update_frame(last_frame_); break;
        case ScopeMode::Vectorscope: vectorscope_scope_->update_frame(last_frame_); break;
        case ScopeMode::Histogram: histogram_scope_->update_frame(last_frame_); break;
        case ScopeMode::CIE:
            chromaticity_->set_frame(last_frame_);
            break;
    }
}

void ScopesPanel::update_frame(canvas::core::RenderFramePtr frame) {
    last_frame_ = std::move(frame);
    feed_frame_to_page();
}

}  // namespace canvas::gui