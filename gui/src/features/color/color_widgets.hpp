#pragma once

// Color-page interaction widgets (Milestone 0 UX scaffold): the Resolve-style
// wheels / curves / scopes panels, all self-contained and theme-aware. These
// only drive their own visual state for now — no grading math is claimed to be
// correct until the core grade model lands (see color.md §3.6 M0).

#include <QColor>
#include <QVector>
#include <QWidget>

#include "canvas/core/media/frame.hpp"
#include "features/color/scopes/common/scope_common.hpp"

class QComboBox;
class QDoubleSpinBox;
class QHBoxLayout;
class QLabel;
class QSlider;
class QStackedWidget;
class QToolButton;

namespace canvas::gui {

class ChromaticityWidget;
class HistogramScope;
class ParadeScope;
class VectorscopeScope;
class WaveformScope;

// The recurring "label + value + slider + reset" triplet that every Resolve
// grading adjustment uses (design spec §Patterns-1). Label sits top-left, the
// value control drives the slider and vice-versa, and the reset arrow returns
// to `reset_value`. All three write through the same `value_changed` signal.
class ToneField : public QWidget {
    Q_OBJECT
public:
    ToneField(const QString& label, double lo, double hi, double value = 0.0,
              double reset_value = 0.0, QWidget* parent = nullptr);
    void set_value(double value);
    [[nodiscard]] double value() const;

signals:
    void value_changed(double value);
    void reset_clicked();

private slots:
    void slider_moved(int pos);
    void spin_changed(double value);

private:
    void sync_slider_from_spin();
    QDoubleSpinBox* spin_ = nullptr;
    QSlider* slider_ = nullptr;
    QLabel* label_ = nullptr;
    double lo_ = 0.0;
    double hi_ = 1.0;
    double reset_value_ = 0.0;
    bool syncing_ = false;
};

// Circular hue/saturation disc with a draggable position marker. The wheel is a
// "where is the current adjustment pointing" proxy — it exposes normalized
// xy (-1..1, x = right = red, y = up = red too, matching the hue ring) but does
// not yet map into a real grade.
class ColorWheelWidget : public QWidget {
    Q_OBJECT
public:
    explicit ColorWheelWidget(QWidget* parent = nullptr);

    void set_xy(const QPointF& xy);
    [[nodiscard]] QPointF xy() const { return xy_; }
    void set_active(bool on) { active_ = on; update(); }

signals:
    void xy_changed(const QPointF& xy);

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;

private:
    [[nodiscard]] QRectF disc_rect() const;
    [[nodiscard]] QPointF pos_to_xy(const QPointF& pos) const;
    [[nodiscard]] QPointF xy_to_pos(const QPointF& xy) const;
    QPointF xy_ = QPointF(0.0, 0.0);
    bool active_ = false;
    bool dragging_ = false;
};

// Primaries panel: header (title + arrow/dot pagination) over four wheels.
// Page 0 = Lift | Gamma | Gain | Offset, page 1 = the HDR-variant
// Dark | Shadow | Light | Global wheel names (design spec §Layout-3). Below the
// wheels sit the shared tonal rows (temp/tint/contrast/pivot, then the color
// boost / saturation / hue / lum-mix row).
class ColorWheelsPanel : public QWidget {
    Q_OBJECT
public:
    explicit ColorWheelsPanel(QWidget* parent = nullptr);

private:
    void set_page(int page);
    QLabel* title_ = nullptr;
    QToolButton* prev_ = nullptr;
    QToolButton* next_ = nullptr;
    QLabel* dots_ = nullptr;
    QVector<ColorWheelWidget*> wheels_;
    QVector<QLabel*> wheel_captions_;
    int page_ = 0;
};

// Parametric curve editor: identity diagonal + optional editable control points
// + a placeholder histogram veil. Click inserts a point, drag moves it,
// double-click removes it, right-click resets the curve. Points are stored
// normalized (0..1 on both axes).
class CurveEditor : public QWidget {
    Q_OBJECT
public:
    explicit CurveEditor(QWidget* parent = nullptr);

    void set_points(const QVector<QPointF>& points);
    [[nodiscard]] QVector<QPointF> points() const { return points_; }
    void set_tint(const QColor& tint);

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;

private:
    [[nodiscard]] QRectF plot_rect() const;
    [[nodiscard]] QPointF to_plot(const QPointF& p) const;
    int hit_point(const QPointF& pos) const;
    QVector<QPointF> points_;
    QColor tint_;
    int drag_index_ = -1;
};

// Curves panel: header (curve dropdown + channel group + polygon toggle) above
// the custom graph, and a soft-clip row (Shadows / Highlights sliders).
class CurvesPanel : public QWidget {
    Q_OBJECT
public:
    explicit CurvesPanel(QWidget* parent = nullptr);

private:
    void set_channel(int channel);
    CurveEditor* editor_ = nullptr;
    QToolButton* shadows_reset_ = nullptr;
    QToolButton* highlights_reset_ = nullptr;
};

// Scopes panel: header with the mode dropdown (+ per-mode sub-display dropdown)
// over one page per scope. Each scope is a frame-fed real widget in its own
// folder (scope_common.hpp + the per-scope classes); the stacked page index
// equals the ScopeMode value.
class ScopesPanel : public QWidget {
    Q_OBJECT
public:
    explicit ScopesPanel(QWidget* parent = nullptr);

    // Feed the frame the preview viewer just presented (SequenceController::
    // frame_ready); forwards to the visible frame-fed scope.
    void update_frame(canvas::core::RenderFramePtr frame);

private:
    void set_mode(ScopeMode mode);
    void set_sub_display(int index);
    void feed_frame_to_page();

    ScopeMode mode_ = ScopeMode::Parade;
    WaveformScope* waveform_scope_ = nullptr;
    ParadeScope* parade_scope_ = nullptr;
    VectorscopeScope* vectorscope_scope_ = nullptr;
    HistogramScope* histogram_scope_ = nullptr;
    ChromaticityWidget* chromaticity_ = nullptr;
    QComboBox* display_sub_ = nullptr;
    QWidget* vector_opts_ = nullptr;
    QComboBox* vec_trace_ = nullptr;
    QSlider* sens_slider_ = nullptr;
    QToolButton* zoom2x_btn_ = nullptr;
    QStackedWidget* stack_ = nullptr;
    canvas::core::RenderFramePtr last_frame_;
};

}  // namespace canvas::gui