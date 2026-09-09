#include "features/color/color_widgets.hpp"
#include <QAbstractSpinBox>
#include <QComboBox>
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
#include <QSlider>
#include <QStackedWidget>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <utility>

#include "UX/theme.hpp"
#include "features/color/scopes/chromaticity/chromaticity_widget.hpp"
#include "features/color/scopes/histogram/histogram_scope.hpp"
#include "features/color/scopes/parade/parade_scope.hpp"
#include "features/color/scopes/vectorscope/vectorscope_scope.hpp"
#include "features/color/scopes/waveform/waveform_scope.hpp"

namespace canvas::gui {

// ── ToneField ────────────────────────────────────────────────────────────────

ToneField::ToneField(const QString& label, double lo, double hi, double value,
                     double reset_value, QWidget* parent)
    : QWidget(parent), lo_(lo), hi_(hi), reset_value_(reset_value) {
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(2);

    label_ = new QLabel(label, this);
    apply_theme_style(label_, [] {
        return QStringLiteral("color: %1; font-size: 11px;")
            .arg(css(tokens().ink_muted));
    });
    root->addWidget(label_);

    auto* row = new QHBoxLayout;
    row->setContentsMargins(0, 0, 0, 0);
    row->setSpacing(4);

    spin_ = new QDoubleSpinBox(this);
    spin_->setRange(lo, hi);
    spin_->setValue(value);
    spin_->setDecimals(2);
    spin_->setButtonSymbols(QAbstractSpinBox::NoButtons);
    spin_->setFixedWidth(58);
    apply_theme_style(spin_, [] {
        const ThemeTokens& t = tokens();
        return QStringLiteral(
            "QDoubleSpinBox { background-color: %1; border: 1px solid %2;"
            " border-radius: 6px; padding: 2px 4px; color: %3; font-size: 11px;"
            " selection-background-color: %4; }"
            "QDoubleSpinBox:focus { border-color: %5; }")
            .arg(css(t.surface_low), css(t.border), css(t.ink), css(t.accent),
                 css(t.accent));
    });

    slider_ = new QSlider(Qt::Horizontal, this);
    slider_->setRange(0, 1000);
    slider_->setValue(static_cast<int>(std::lround(
        (value - lo) / (hi - lo) * 1000.0)));
    slider_->setFixedHeight(16);
    apply_theme_style(slider_, &slider_style);

    auto* reset = new QToolButton(this);
    reset->setIcon(icon("reset"));
    reset->setIconSize(QSize(13, 13));
    reset->setAutoRaise(true);
    reset->setFixedWidth(18);
    reset->setToolTip(tr("Reset to default"));
    apply_theme_style(reset, &flat_tool_style);
    QObject::connect(reset, &QToolButton::clicked, this, [this] {
        set_value(reset_value_);
        emit reset_clicked();
    });

    row->addWidget(spin_);
    row->addWidget(slider_, 1);
    row->addWidget(reset);
    root->addLayout(row);

    QObject::connect(slider_, &QSlider::valueChanged, this,
                     &ToneField::slider_moved);
    QObject::connect(spin_, qOverload<double>(&QDoubleSpinBox::valueChanged),
                     this, &ToneField::spin_changed);
}

double ToneField::value() const {
    return spin_ ? spin_->value() : lo_;
}

void ToneField::set_value(double value) {
    if (!spin_) return;
    syncing_ = true;
    spin_->setValue(std::clamp(value, lo_, hi_));
    slider_->setValue(static_cast<int>(std::lround(
        (spin_->value() - lo_) / (hi_ - lo_) * 1000.0)));
    syncing_ = false;
}

void ToneField::slider_moved(int pos) {
    if (syncing_ || !spin_) return;
    syncing_ = true;
    const double v = lo_ + (hi_ - lo_) * pos / 1000.0;
    spin_->setValue(v);
    syncing_ = false;
    emit value_changed(v);
}

void ToneField::spin_changed(double value) {
    sync_slider_from_spin();
    if (!syncing_) emit value_changed(value);
}

void ToneField::sync_slider_from_spin() {
    if (!slider_ || !spin_) return;
    syncing_ = true;
    slider_->setValue(static_cast<int>(std::lround(
        (spin_->value() - lo_) / (hi_ - lo_) * 1000.0)));
    syncing_ = false;
}

// ── ColorWheelWidget ─────────────────────────────────────────────────────────

ColorWheelWidget::ColorWheelWidget(QWidget* parent) : QWidget(parent) {
    setCursor(Qt::PointingHandCursor);
    setMinimumSize(104, 104);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
}

QRectF ColorWheelWidget::disc_rect() const {
    const qreal r = (std::min(width(), height()) - 6.0) / 2.0;
    return QRectF(width() / 2.0 - r, height() / 2.0 - r, 2.0 * r, 2.0 * r);
}

QPointF ColorWheelWidget::pos_to_xy(const QPointF& pos) const {
    const QPointF c = disc_rect().center();
    QPointF d(pos.x() - c.x(), pos.y() - c.y());
    const qreal len = std::hypot(d.x(), d.y());
    if (len > 0.0) d /= len;
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

void ColorWheelWidget::paintEvent(QPaintEvent* event) {
    Q_UNUSED(event);
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const ThemeTokens& t = tokens();

    const QRectF disc = disc_rect();
    const qreal r = disc.width() / 2.0;
    const QPointF c = disc.center();

    // Hue ring: 56 hue segments around the band at full saturation.
    static constexpr int kSegments = 56;
    QPen ring_pen;
    ring_pen.setWidthF(std::max(6.0, r * 0.08));
    ring_pen.setCapStyle(Qt::FlatCap);
    for (int i = 0; i < kSegments; ++i) {
        const double a0 = 2.0 * M_PI * i / kSegments;
        const double a1 = 2.0 * M_PI * (i + 1) / kSegments;
        ring_pen.setColor(QColor::fromHsvF(i / 56.0, 1.0, 0.62));
        p.setPen(ring_pen);
        p.drawLine(c + QPointF(std::cos(a0), std::sin(a0)) * (r - 3.0),
                   c + QPointF(std::cos(a1), std::sin(a1)) * (r - 3.0));
    }

    // Inner well.
    const qreal inset = std::clamp(r * 0.06, 4.0, 10.0);
    QRadialGradient well(c, r, c, 0.35 * r);
    well.setColorAt(0.0, t.surface_low);
    well.setColorAt(1.0, t.surface);
    p.setPen(QPen(t.border, 1.0));
    p.setBrush(well);
    p.drawEllipse(disc.adjusted(inset, inset, -inset, -inset));

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

// ── ColorWheelsPanel ─────────────────────────────────────────────────────────

ColorWheelsPanel::ColorWheelsPanel(QWidget* parent) : QWidget(parent) {
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(10, 10, 10, 10);
    root->setSpacing(8);

    // Header: title with arrow/dot pagination (spec recurring pattern).
    auto* header = new QWidget(this);
    auto* header_row = new QHBoxLayout(header);
    header_row->setContentsMargins(0, 0, 0, 0);
    header_row->setSpacing(4);

    title_ = new QLabel(header);
    apply_theme_style(title_, [] {
        const ThemeTokens& t = tokens();
        return QStringLiteral("color: %1; font-size: 12px; font-weight: 600;")
            .arg(css(t.ink));
    });
    header_row->addWidget(title_, 1);

    prev_ = new QToolButton(header);
    prev_->setIcon(icon("chevron_left"));
    prev_->setIconSize(QSize(14, 14));
    prev_->setAutoRaise(true);
    apply_theme_style(prev_, &flat_tool_style);
    header_row->addWidget(prev_);

    dots_ = new QLabel(header);
    dots_->setAlignment(Qt::AlignCenter);
    dots_->setFixedWidth(26);
    apply_theme_style(dots_, [] {
        return QStringLiteral("color: %1; font-size: 9px;")
            .arg(css(tokens().ink_muted));
    });
    header_row->addWidget(dots_);

    next_ = new QToolButton(header);
    next_->setIcon(icon("chevron_right"));
    next_->setIconSize(QSize(14, 14));
    next_->setAutoRaise(true);
    apply_theme_style(next_, &flat_tool_style);
    header_row->addWidget(next_);

    root->addWidget(header);

    // Four big wheels in a 2×2 grid, each with a caption underneath. A single
    // row of tiny wheels reads like miniature beads; two rows let every wheel
    // grow to ~60% of the panel width (Resolve's wheels are the primary tool).
    auto* wheels_row = new QWidget(this);
    auto* wheels_grid = new QGridLayout(wheels_row);
    wheels_grid->setContentsMargins(0, 0, 0, 0);
    wheels_grid->setSpacing(8);
    for (int i = 0; i < 4; ++i) {
        auto* cell = new QWidget(wheels_row);
        auto* cell_layout = new QVBoxLayout(cell);
        cell_layout->setContentsMargins(0, 0, 0, 0);
        cell_layout->setSpacing(2);
        auto* wheel = new ColorWheelWidget(cell);
        wheel_captions_.append(new QLabel(cell));
        apply_theme_style(wheel_captions_.back(), [] {
            return QStringLiteral("color: %1; font-size: 11px; font-weight: 550;")
                .arg(css(tokens().ink_muted));
        });
        wheel_captions_.back()->setAlignment(Qt::AlignHCenter);
        cell_layout->addWidget(wheel, 1);
        cell_layout->addWidget(wheel_captions_.back());
        wheels_grid->addWidget(cell, i / 2, i % 2);
        wheels_.append(wheel);
    }
    for (int col = 0; col < 2; ++col) wheels_grid->setColumnStretch(col, 1);
    for (int row = 0; row < 2; ++row) wheels_grid->setRowStretch(row, 1);
    root->addWidget(wheels_row, 1);

    // Shared tonal row: temperature / tint / contrast / pivot.
    auto* tonal_row = new QWidget(this);
    auto* tonal_layout = new QHBoxLayout(tonal_row);
    tonal_layout->setContentsMargins(0, 0, 0, 0);
    tonal_layout->setSpacing(10);
    tonal_layout->addWidget(new ToneField(tr("Temp"), -100.0, 100.0, 0.0, 0.0, tonal_row), 1);
    tonal_layout->addWidget(new ToneField(tr("Tint"), -100.0, 100.0, 0.0, 0.0, tonal_row), 1);
    tonal_layout->addWidget(new ToneField(tr("Contrast"), -100.0, 100.0, 0.0, 0.0, tonal_row), 1);
    tonal_layout->addWidget(new ToneField(tr("Pivot"), -100.0, 100.0, 0.0, 0.0, tonal_row), 1);
    root->addWidget(tonal_row);

    // Shared chroma row: boost / saturation / hue / lum mix.
    auto* chroma_row = new QWidget(this);
    auto* chroma_layout = new QHBoxLayout(chroma_row);
    chroma_layout->setContentsMargins(0, 0, 0, 0);
    chroma_layout->setSpacing(10);
    chroma_layout->addWidget(new ToneField(tr("Color Boost"), -100.0, 100.0, 0.0, 0.0, chroma_row), 1);
    chroma_layout->addWidget(new ToneField(tr("Saturation"), -100.0, 100.0, 0.0, 0.0, chroma_row), 1);
    chroma_layout->addWidget(new ToneField(tr("Hue"), -180.0, 180.0, 0.0, 0.0, chroma_row), 1);
    chroma_layout->addWidget(new ToneField(tr("Lum Mix"), -100.0, 100.0, 0.0, 0.0, chroma_row), 1);
    root->addWidget(chroma_row);

    QObject::connect(prev_, &QToolButton::clicked, this,
                     [this] { set_page((page_ + 1) % 2); });
    QObject::connect(next_, &QToolButton::clicked, this,
                     [this] { set_page((page_ + 1) % 2); });

    set_page(0);
}

void ColorWheelsPanel::set_page(int page) {
    page_ = page;
    static const char* const kNames[2][4] = {
        {"Lift", "Gamma", "Gain", "Offset"},
        {"Dark", "Shadow", "Light", "Global"},
    };
    const QString title = tr(page_ == 0 ? "Primaries - Color Wheels"
                                        : "HDR Color Wheels");
    title_->setText(title);
    for (int i = 0; i < 4; ++i) {
        wheel_captions_[i]->setText(tr(kNames[page_][i]));
        wheels_[i]->set_active(page_ != 0);  // HDR wheels read as "lit"
    }
    dots_->setText(QStringLiteral("%1 %2").arg(page_ == 0 ? "\u25CF" : "\u25CB",
                                               page_ == 1 ? "\u25CF" : "\u25CB"));
}

// ── CurveEditor ──────────────────────────────────────────────────────────────

CurveEditor::CurveEditor(QWidget* parent) : QWidget(parent) {
    tint_ = QColor(255, 255, 255);
    setMinimumSize(180, 140);
    setCursor(Qt::CrossCursor);
}

QRectF CurveEditor::plot_rect() const {
    return QRectF(8.0, 8.0, width() - 16.0, height() - 16.0);
}

QPointF CurveEditor::to_plot(const QPointF& p) const {
    const QRectF r = plot_rect();
    return QPointF(r.left() + p.x() * r.width(), r.top() + (1.0 - p.y()) * r.height());
}

int CurveEditor::hit_point(const QPointF& pos) const {
    const QRectF r = plot_rect();
    for (int i = 0; i < points_.size(); ++i) {
        const QPointF pp = to_plot(points_[i]);
        if (QLineF(pp, pos).length() <= 9.0) return i;
    }
    return -1;
}

void CurveEditor::set_points(const QVector<QPointF>& points) {
    points_ = points;
    update();
}

void CurveEditor::set_tint(const QColor& tint) {
    tint_ = tint;
    update();
}

void CurveEditor::paintEvent(QPaintEvent* event) {
    Q_UNUSED(event);
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const ThemeTokens& t = tokens();
    const QRectF r = plot_rect();

    // Panel well.
    p.setBrush(t.surface_low);
    p.setPen(QPen(t.border, 1.0));
    p.drawRoundedRect(r, 8.0, 8.0);

    // Grid: quarters.
    p.setPen(QPen(with_alpha(t.ink, 22), 1.0));
    p.drawLine(QPointF(r.left(), r.center().y()), QPointF(r.right(), r.center().y()));
    p.drawLine(QPointF(r.center().x(), r.top()), QPointF(r.center().x(), r.bottom()));

    // Bounding frame ticks.
    p.setPen(QPen(with_alpha(t.ink, 40), 1.0));
    p.drawRect(r);

    // Placeholder histogram veil (static, deterministic).
    p.setClipRect(r);
    QPen data_pen(QColor(255, 255, 255));
    for (double x = 0.0; x <= 1.0; x += 0.02) {
        const double luma = 0.52 + 0.30 * std::sin(x * 6.283 + 1.2) +
                            0.16 * std::sin(x * 12.566 + 0.4);
        const double v = std::clamp(luma, 0.05, 0.95);
        data_pen.setColor(with_alpha(QColor(255, 255, 255), 28));
        p.setPen(data_pen);
        p.drawLine(to_plot(QPointF(x, v)), QPointF(to_plot(QPointF(x, v)).x(), r.bottom()));
    }

    // Curve itself: from identity to the editable points.
    QVector<QPointF> poly;
    poly.reserve(points_.size() + 2);
    poly.append(QPointF(0.0, 0.0));
    QVector<QPointF> sorted = points_;
    std::sort(sorted.begin(), sorted.end(),
              [](const QPointF& a, const QPointF& b) { return a.x() < b.x(); });
    for (const QPointF& pt : sorted) poly.append(pt);
    poly.append(QPointF(1.0, 1.0));

    QPainterPath path;
    path.moveTo(to_plot(poly.front()));
    for (int i = 1; i < poly.size(); ++i) path.lineTo(to_plot(poly[i]));

    p.setPen(QPen(tint_, 1.8));
    p.drawPath(path);

    // Control points.
    for (const QPointF& pt : sorted) {
        p.setPen(QPen(t.surface_highest, 1.0));
        p.setBrush(t.accent);
        p.drawEllipse(to_plot(pt), 3.5, 3.5);
    }
    p.setClipping(false);
}

void CurveEditor::mousePressEvent(QMouseEvent* event) {
    const QPointF pos = event->position();
    const int hit = hit_point(pos);
    if (event->button() == Qt::LeftButton) {
        if (hit >= 0) {
            drag_index_ = hit;
        } else {
            const QRectF r = plot_rect();
            QPointF p((pos.x() - r.left()) / r.width(), 1.0 - (pos.y() - r.top()) / r.height());
            p.setX(std::clamp(p.x(), 0.02, 0.98));
            p.setY(std::clamp(p.y(), 0.02, 0.98));
            points_.append(p);
            std::sort(points_.begin(), points_.end(),
                      [](const QPointF& a, const QPointF& b) { return a.x() < b.x(); });
            drag_index_ = hit_point(pos);
            update();
        }
    } else if (event->button() == Qt::RightButton) {
        points_.clear();
        update();
    }
}

void CurveEditor::mouseMoveEvent(QMouseEvent* event) {
    if (drag_index_ < 0 || drag_index_ >= points_.size()) return;
    const QRectF r = plot_rect();
    QPointF p((event->position().x() - r.left()) / r.width(),
              1.0 - (event->position().y() - r.top()) / r.height());
    p.setX(std::clamp(p.x(), 0.02, 0.98));
    p.setY(std::clamp(p.y(), 0.02, 0.98));
    points_[drag_index_] = p;
    std::sort(points_.begin(), points_.end(),
              [](const QPointF& a, const QPointF& b) { return a.x() < b.x(); });
    drag_index_ = hit_point(event->position());
    update();
}

void CurveEditor::mouseDoubleClickEvent(QMouseEvent* event) {
    const int hit = hit_point(event->position());
    if (hit >= 0) {
        points_.removeAt(hit);
        update();
    }
}

// ── CurvesPanel ──────────────────────────────────────────────────────────────

CurvesPanel::CurvesPanel(QWidget* parent) : QWidget(parent) {
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(10, 10, 10, 10);
    root->setSpacing(8);

    auto* header = new QWidget(this);
    auto* header_row = new QHBoxLayout(header);
    header_row->setContentsMargins(0, 0, 0, 0);
    header_row->setSpacing(4);

    auto* title = new QLabel(tr("Curves - Custom"), header);
    apply_theme_style(title, [] {
        const ThemeTokens& t = tokens();
        return QStringLiteral("color: %1; font-size: 12px; font-weight: 600;")
            .arg(css(t.ink));
    });
    header_row->addWidget(title, 1);

    auto* curve_combo = new QComboBox(header);
    curve_combo->addItems({tr("Custom"), tr("Luma vs Sat"), tr("Hue vs Hue"),
                           tr("Hue vs Sat"), tr("Sat vs Sat")});
    apply_theme_style(curve_combo, &flat_tool_style);
    header_row->addWidget(curve_combo);

    auto* channels = new QWidget(header);
    auto* channels_row = new QHBoxLayout(channels);
    channels_row->setContentsMargins(0, 0, 0, 0);
    channels_row->setSpacing(2);
    const char* const names[] = {"Y", "R", "G", "B"};
    const QColor tints[] = {QColor(230, 230, 230), QColor(0xD9, 0x53, 0x4F),
                            QColor(0x7B, 0xC9, 0x50), QColor(0x4F, 0x86, 0xD9)};
    for (int i = 0; i < 4; ++i) {
        auto* b = new QToolButton(channels);
        b->setText(tr(names[i]));
        b->setCheckable(true);
        b->setChecked(i == 0);
        b->setAutoRaise(true);
        apply_theme_style(b, &outline_pill_style);
        QObject::connect(b, &QToolButton::clicked, this,
                         [this, i, tints, channels, b] {
                             set_channel(i);
                             for (QToolButton* other : channels->findChildren<QToolButton*>())
                                 other->setChecked(other == b);
                         });
        channels_row->addWidget(b);
    }
    header_row->addWidget(channels);
    root->addWidget(header);

    editor_ = new CurveEditor(this);
    root->addWidget(editor_, 1);
    set_channel(0);

    auto* soft = new QWidget(this);
    auto* soft_row = new QHBoxLayout(soft);
    soft_row->setContentsMargins(0, 0, 0, 0);
    soft_row->setSpacing(10);
    soft_row->addWidget(new ToneField(tr("Soft Clip Shadows"), -100.0, 100.0, 0.0, 0.0, soft), 1);
    soft_row->addWidget(new ToneField(tr("Soft Clip Highlights"), -100.0, 100.0, 0.0, 0.0, soft), 1);
    root->addWidget(soft);
}

void CurvesPanel::set_channel(int channel) {
    switch (channel) {
        case 0: editor_->set_tint(QColor(235, 235, 235)); break;
        case 1: editor_->set_tint(QColor(0xD9, 0x53, 0x4F)); break;
        case 2: editor_->set_tint(QColor(0x7B, 0xC9, 0x50)); break;
        default: editor_->set_tint(QColor(0x4F, 0x86, 0xD9)); break;
    }
    editor_->set_points({});
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