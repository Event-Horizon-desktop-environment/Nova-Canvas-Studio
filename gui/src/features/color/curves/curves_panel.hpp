#pragma once

// Curves panel: a real per-channel (Y/R/G/B) parametric curve editor plus a
// four-field soft-clip tone row. Thin view over the headless curve law
// (canvas/core/colorsci/curves.hpp): the editor paints the law itself (samples
// colorsci::eval_curve over the plot), and the panel serializes its editable
// state into a colorsci::CurveParams that the Color page turns into a
// set_clip_grade edit op — same handshake as ColorWheelsPanel.

#include "canvas/core/colorsci/curves.hpp"

#include <QPointF>
#include <QVector>
#include <QWidget>

#include <array>
#include <chrono>
#include <vector>

class QColor;
class QMouseEvent;
class QPaintEvent;

namespace canvas::gui {

class ToneField;

// Parametric curve editor: identity diagonal + optional editable control points
// + an optional luminance-histogram veil. Click inserts a point, drag moves it,
// double-click removes it, right-click resets the curve. Points are stored
// normalized (0..1 on both axes). The painted curve is the actual headless law:
// the same eval_curve spline the evaluator runs, so what you see is what the
// export renders.
class CurveEditor : public QWidget {
    Q_OBJECT
public:
    explicit CurveEditor(QWidget* parent = nullptr);

    void set_points(const QVector<QPointF>& points);
    [[nodiscard]] QVector<QPointF> points() const { return points_; }
    void set_tint(const QColor& tint);
    // Luminance histogram veil behind the curve: one normalized height (0..1)
    // per column. The panel updates this from the presented frame's histogram
    // so the curve editor shadows the current signal like the mockup's.
    void set_veil(const std::vector<float>& col_heights);

signals:
    // The curve law changed during an in-progress edit (drag feed).
    void points_changed();
    // The user finished an edit (drag end / point add / delete / reset).
    void points_committed();

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;

private:
    [[nodiscard]] QRectF plot_rect() const;
    [[nodiscard]] QPointF to_plot(const QPointF& p) const;
    int hit_point(const QPointF& pos) const;

    QVector<QPointF> points_;
    QColor tint_;
    std::vector<float> veil_;
    int drag_index_ = -1;
    // Last plot-space px during a drag, for the per-move delta trace.
    QPointF last_plot_px_;
};

// Curves panel: header (curve dropdown + Y/R/G/B channel group) above the
// editor, and a soft-clip tone row (Low / Low Soft / High Soft / High) below.
// All four channels plus the soft-clip fields serialize into ONE
// colorsci::CurveParams; the channel buttons switch which channel the editor
// edits while the panel keeps each channel's points independent.
class CurvesPanel : public QWidget {
    Q_OBJECT
public:
    using CurveParams = canvas::core::colorsci::CurveParams;

    explicit CurvesPanel(QWidget* parent = nullptr);

    // Load/save the controller state (used when a clip's grade round-trips).
    void set_params(const CurveParams& params);
    [[nodiscard]] CurveParams params() const;

    // Luminance veil for the active editor (0..1 per histogram column).
    void set_veil(const std::vector<float>& col_heights);

signals:
    // Live curve/soft-clip movement (no undo boundary). Drives previews.
    void curves_preview();
    // A drag/release/reset/soft-clip commit finished: the panel state changed
    // and the Color page should commit ONE undoable set_clip_grade.
    void curves_committed(const canvas::core::colorsci::CurveParams& params);

private:
    // Channel storage order matches colorsci::CurveChannel.
    enum Channel : int { kLuma = 0, kRed = 1, kGreen = 2, kBlue = 3 };

    void set_channel(int channel);
    void save_active_channel();
    void editor_points_changed();
    void editor_points_committed();
    void tone_changed(int field, double value);

    CurveEditor* editor_ = nullptr;
    // Soft-clip tone fields, index == SoftClip member order (low, low_soft,
    // high_soft, high). Value shown in percent (0..100); defaults low/low_soft/
    // high_soft = 0, high = 100.
    QVector<ToneField*> tone_fields_;
    std::array<QVector<QPointF>, 4> channel_points_;
    int active_channel_ = kLuma;
    bool syncing_ = false;

    // Interaction-tap rate limiter, same cadence + purpose as the wheel panel's.
    [[nodiscard]] bool interaction_log_gate();
    std::chrono::steady_clock::time_point last_interaction_log_{};
};

}  // namespace canvas::gui