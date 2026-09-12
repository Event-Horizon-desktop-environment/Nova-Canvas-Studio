#include "features/color/curves/curves_panel.hpp"

#include <algorithm>
#include <cmath>
#include <utility>

#include <QComboBox>
#include <QFont>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineF>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QToolButton>
#include <QVBoxLayout>

#include "UX/theme.hpp"
#include "canvas/core/util/color_log.hpp"
#include "features/color/color_widgets.hpp"

namespace canvas::gui {

using canvas::core::colorsci::eval_curve;

namespace {
// Extreme curve-gesture geometry: plot px -> normalized point (0..1) on both
// axes, clamped to the editor's 2% inset. One line per mouse event in the
// archive, so color.log captures the exact path every control point takes.
QPointF plot_px_to_p01(const QRectF& r, const QPointF& pos) {
    QPointF p((pos.x() - r.left()) / r.width(), 1.0 - (pos.y() - r.top()) / r.height());
    p.setX(std::clamp(p.x(), 0.02, 0.98));
    p.setY(std::clamp(p.y(), 0.02, 0.98));
    return p;
}
}  // namespace

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

void CurveEditor::set_veil(const std::vector<float>& col_heights) {
    veil_ = col_heights;
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

    p.setClipRect(r);

    // Luminance histogram veil: per-column bars rising from the bottom, driven
    // by the panel's per-column luma profile (0..1). Empty vector = no veil.
    if (!veil_.empty()) {
        const double bar_w = r.width() / static_cast<double>(veil_.size());
        p.setPen(Qt::NoPen);
        for (std::size_t i = 0; i < veil_.size(); ++i) {
            const float v = std::clamp(veil_[i], 0.0f, 1.0f);
            if (v <= 0.0f) continue;
            const double x = r.left() + static_cast<double>(i) * bar_w;
            const double w = std::max(1.0, bar_w - 0.5);
            p.setBrush(with_alpha(QColor(255, 255, 255), 26));
            p.drawRect(QRectF(x, r.bottom() - v * r.height(), w, v * r.height()));
        }
    }

    // Curve itself: the headless law sampled over the plot, so the editor is a
    // faithful view of what the evaluator runs (empty points = identity line).
    QVector<QPointF> sorted = points_;
    std::sort(sorted.begin(), sorted.end(),
              [](const QPointF& a, const QPointF& b) { return a.x() < b.x(); });
    std::vector<canvas::core::colorsci::CurvePoint> knots;
    knots.reserve(sorted.size());
    for (const QPointF& pt : sorted) {
        knots.push_back({static_cast<float>(pt.x()), static_cast<float>(pt.y())});
    }

    QPainterPath path;
    constexpr int kSamples = 192;
    for (int i = 0; i <= kSamples; ++i) {
        const float x = static_cast<float>(i) / kSamples;
        const float y = eval_curve(knots, x);
        const QPointF pos = to_plot(QPointF(x, y));
        if (i == 0) {
            path.moveTo(pos);
        } else {
            path.lineTo(pos);
        }
    }

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
            last_plot_px_ = pos;
            CANVAS_COLOR_LOG(
                "[curve] press GRAB point pos=(%.0f,%.0f) idx=%d pts=%d p=(%.3f,%.3f)",
                pos.x(), pos.y(), hit, points_.size(),
                static_cast<double>(points_[hit].x()),
                static_cast<double>(points_[hit].y()));
        } else {
            const QRectF r = plot_rect();
            QPointF p = plot_px_to_p01(r, pos);
            points_.append(p);
            std::sort(points_.begin(), points_.end(),
                      [](const QPointF& a, const QPointF& b) { return a.x() < b.x(); });
            drag_index_ = hit_point(pos);
            last_plot_px_ = pos;
            CANVAS_COLOR_LOG(
                "[curve] press ADD point pos=(%.0f,%.0f) p=(%.3f,%.3f) pts=%d "
                "drag_idx=%d",
                pos.x(), pos.y(), static_cast<double>(p.x()),
                static_cast<double>(p.y()), points_.size(), drag_index_);
        }
        emit points_changed();
        update();
    } else if (event->button() == Qt::RightButton) {
        if (!points_.isEmpty()) {
            CANVAS_COLOR_LOG(
                "[curve] press RIGHT-CLICK reset pts_before=%d pos=(%.0f,%.0f)",
                points_.size(), pos.x(), pos.y());
            points_.clear();
            emit points_changed();
            emit points_committed();
            update();
        }
    }
}

void CurveEditor::mouseMoveEvent(QMouseEvent* event) {
    if (drag_index_ < 0 || drag_index_ >= points_.size()) return;
    const QRectF r = plot_rect();
    const QPointF pos = event->position();
    QPointF p = plot_px_to_p01(r, pos);
    const QPointF prev_p = points_[drag_index_];
    const QPointF d_plot = pos - last_plot_px_;
    last_plot_px_ = pos;
    points_[drag_index_] = p;
    std::sort(points_.begin(), points_.end(),
              [](const QPointF& a, const QPointF& b) { return a.x() < b.x(); });
    drag_index_ = hit_point(event->position());
    CANVAS_COLOR_LOG(
        "[curve] move pos=(%.0f,%.0f) p=(%.3f,%.3f) prev=(%.3f,%.3f) dpx=(%.1f,%.1f) "
        "pts=%d drag_idx=%d",
        pos.x(), pos.y(), static_cast<double>(p.x()), static_cast<double>(p.y()),
        static_cast<double>(prev_p.x()), static_cast<double>(prev_p.y()),
        static_cast<double>(d_plot.x()), static_cast<double>(d_plot.y()),
        points_.size(), drag_index_);
    emit points_changed();
    update();
}

void CurveEditor::mouseReleaseEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton && drag_index_ >= 0) {
        const int released_idx = drag_index_;
        drag_index_ = -1;
        CANVAS_COLOR_LOG(
            "[curve] release pos=(%.0f,%.0f) idx=%d pts=%d -> committed",
            event->position().x(), event->position().y(), released_idx, points_.size());
        emit points_committed();
    }
    QWidget::mouseReleaseEvent(event);
}

void CurveEditor::mouseDoubleClickEvent(QMouseEvent* event) {
    const int hit = hit_point(event->position());
    if (hit >= 0) {
        CANVAS_COLOR_LOG(
            "[curve] double-click DELETE idx=%d p=(%.3f,%.3f) pts_before=%d",
            hit, static_cast<double>(points_[hit].x()),
            static_cast<double>(points_[hit].y()), points_.size());
        points_.removeAt(hit);
        emit points_changed();
        emit points_committed();
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
    for (int i = 0; i < 4; ++i) {
        auto* b = new QToolButton(channels);
        b->setText(tr(names[i]));
        b->setCheckable(true);
        b->setChecked(i == 0);
        b->setAutoRaise(true);
        apply_theme_style(b, &outline_pill_style);
        QObject::connect(b, &QToolButton::clicked, this,
                         [this, i, channels, b] {
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
    set_channel(kLuma);

    // Soft-clip tone row: field order == SoftClip member order. Stored/displayed
    // in percent (Law uses 0..1); High defaults to 100 (top rail untouched).
    auto* soft = new QWidget(this);
    auto* soft_grid = new QGridLayout(soft);
    soft_grid->setContentsMargins(0, 0, 0, 0);
    soft_grid->setSpacing(6);
    auto make_tone = [this, soft](const QString& label, double value, double reset) {
        auto* f = new ToneField(label, 0.0, 100.0, value, reset, soft,
                                ToneFieldMode::kFieldReset);
        return f;
    };
    auto* low = make_tone(tr("Low"), 0.0, 0.0);
    auto* low_soft = make_tone(tr("Low Soft"), 0.0, 0.0);
    auto* high_soft = make_tone(tr("High Soft"), 0.0, 0.0);
    auto* high = make_tone(tr("High"), 100.0, 100.0);
    tone_fields_ = {low, low_soft, high_soft, high};
    for (int i = 0; i < tone_fields_.size(); ++i) {
        QObject::connect(tone_fields_[i], &ToneField::value_changed, this,
                         [this, i](double v) { tone_changed(i, v); });
        QObject::connect(tone_fields_[i], &ToneField::reset_clicked, this,
                         [this, i] { tone_changed(i, tone_fields_[i]->value()); });
    }
    soft_grid->addWidget(low, 0, 0);
    soft_grid->addWidget(low_soft, 0, 1);
    soft_grid->addWidget(high_soft, 1, 0);
    soft_grid->addWidget(high, 1, 1);
    root->addWidget(soft);

    QObject::connect(editor_, &CurveEditor::points_changed, this,
                     &CurvesPanel::editor_points_changed);
    QObject::connect(editor_, &CurveEditor::points_committed, this,
                     &CurvesPanel::editor_points_committed);
}

void CurvesPanel::set_channel(int channel) {
    save_active_channel();
    const int from = active_channel_;
    active_channel_ = channel;
    switch (channel) {
        case kLuma: editor_->set_tint(QColor(235, 235, 235)); break;
        case kRed: editor_->set_tint(QColor(0xD9, 0x53, 0x4F)); break;
        case kGreen: editor_->set_tint(QColor(0x7B, 0xC9, 0x50)); break;
        default: editor_->set_tint(QColor(0x4F, 0x86, 0xD9)); break;
    }
    editor_->set_points(channel_points_[channel]);
    qWarning().nospace()
        << "[grade] curve-channel=" << channel
        << " pts=" << channel_points_[channel].size();
    CANVAS_COLOR_LOG(
        "[curve] channel switch %d -> %d pts=[Y:%d R:%d G:%d B:%d]",
        from, channel,
        static_cast<int>(channel_points_[0].size()),
        static_cast<int>(channel_points_[1].size()),
        static_cast<int>(channel_points_[2].size()),
        static_cast<int>(channel_points_[3].size()));
}

void CurvesPanel::save_active_channel() {
    channel_points_[active_channel_] = editor_->points();
    CANVAS_COLOR_LOG(
        "[curve] save-active channel=%d pts=%d",
        active_channel_, static_cast<int>(editor_->points().size()));
}

bool CurvesPanel::interaction_log_gate() {
    const auto now = std::chrono::steady_clock::now();
    if (now - last_interaction_log_ < std::chrono::milliseconds(100)) return false;
    last_interaction_log_ = now;
    return true;
}

void CurvesPanel::editor_points_changed() {
    if (syncing_) return;
    save_active_channel();
    if (interaction_log_gate()) {
        qWarning().nospace()
            << "[grade] curve-drag channel=" << active_channel_
            << " pts=" << channel_points_[active_channel_].size();
    }
    const auto& pts = channel_points_[active_channel_];
    CANVAS_COLOR_LOG(
        "[curve] live-feed channel=%d pts=%d head=(%.3f,%.3f) tail=(%.3f,%.3f)",
        active_channel_, static_cast<int>(pts.size()),
        pts.isEmpty() ? 0.0 : static_cast<double>(pts.first().x()),
        pts.isEmpty() ? 0.0 : static_cast<double>(pts.first().y()),
        pts.isEmpty() ? 0.0 : static_cast<double>(pts.last().x()),
        pts.isEmpty() ? 0.0 : static_cast<double>(pts.last().y()));
    emit curves_preview();
}

void CurvesPanel::editor_points_committed() {
    if (syncing_) return;
    save_active_channel();
    // Always-on committed trace: point add/remove/drag-end/reset per channel,
    // plus the resulting point count so a reset (→ 0 points) is legible.
    qWarning().nospace()
        << "[grade] curve-commit channel=" << active_channel_
        << " pts=" << channel_points_[active_channel_].size();
    QString dump;
    for (int ch = 0; ch < 4; ++ch) {
        dump += QStringLiteral("ch%1=%2;").arg(ch).arg(channel_points_[ch].size());
        for (const QPointF& pt : channel_points_[ch]) {
            dump += QStringLiteral("(%1,%2)").arg(QString::number(pt.x(), 'f', 2),
                                                  QString::number(pt.y(), 'f', 2));
        }
    }
    CANVAS_COLOR_LOG(
        "[curve] committed channel=%d pts=%d dump=%s", active_channel_,
        static_cast<int>(channel_points_[active_channel_].size()),
        dump.toStdString().c_str());
    emit curves_committed(params());
    emit curves_preview();
}

void CurvesPanel::tone_changed(int field, double value) {
    Q_UNUSED(value);
    if (syncing_) return;
    if (interaction_log_gate()) {
        qWarning().nospace()
            << "[grade] curve-tone field=" << field
            << " value=" << QString::number(value, 'f', 2);
    }
    CANVAS_COLOR_LOG(
        "[curve] tone field=%d value=%.2f soft=(low=%.1f lowsoft=%.1f highsoft=%.1f "
        "high=%.1f)",
        field, value, tone_fields_[0]->value(), tone_fields_[1]->value(),
        tone_fields_[2]->value(), tone_fields_[3]->value());
    // Soft-clip rows commit on every change, matching the wheels' tone rows.
    emit curves_committed(params());
}

CurvesPanel::CurveParams CurvesPanel::params() const {
    CurveParams out;
    for (int i = 0; i < 4; ++i) {
        out.channels[i].reserve(channel_points_[i].size());
        for (const QPointF& pt : channel_points_[i]) {
            out.channels[i].push_back(
                {static_cast<float>(pt.x()), static_cast<float>(pt.y())});
        }
    }
    out.soft_clip.low = static_cast<float>(tone_fields_[0]->value() / 100.0);
    out.soft_clip.low_soft = static_cast<float>(tone_fields_[1]->value() / 100.0);
    out.soft_clip.high_soft = static_cast<float>(tone_fields_[2]->value() / 100.0);
    out.soft_clip.high = static_cast<float>(tone_fields_[3]->value() / 100.0);
    return out;
}

void CurvesPanel::set_params(const CurveParams& params) {
    syncing_ = true;
    for (int i = 0; i < 4; ++i) {
        channel_points_[i].clear();
        channel_points_[i].reserve(params.channels[i].size());
        for (const auto& cp : params.channels[i]) {
            channel_points_[i].append(QPointF(cp.x, cp.y));
        }
    }
    tone_fields_[0]->set_value(params.soft_clip.low * 100.0);
    tone_fields_[1]->set_value(params.soft_clip.low_soft * 100.0);
    tone_fields_[2]->set_value(params.soft_clip.high_soft * 100.0);
    tone_fields_[3]->set_value(params.soft_clip.high * 100.0);
    editor_->set_points(channel_points_[active_channel_]);
    syncing_ = false;
    CANVAS_COLOR_LOG(
        "[curve] load pts=[Y:%d R:%d G:%d B:%d] soft=(%.1f,%.1f,%.1f,%.1f)",
        static_cast<int>(channel_points_[0].size()),
        static_cast<int>(channel_points_[1].size()),
        static_cast<int>(channel_points_[2].size()),
        static_cast<int>(channel_points_[3].size()),
        static_cast<double>(params.soft_clip.low * 100.0),
        static_cast<double>(params.soft_clip.low_soft * 100.0),
        static_cast<double>(params.soft_clip.high_soft * 100.0),
        static_cast<double>(params.soft_clip.high * 100.0));
}

void CurvesPanel::set_veil(const std::vector<float>& col_heights) {
    editor_->set_veil(col_heights);
}

}  // namespace canvas::gui