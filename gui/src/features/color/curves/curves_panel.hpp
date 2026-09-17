#pragma once

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

class CurveEditor : public QWidget {
    Q_OBJECT
public:
    explicit CurveEditor(QWidget* parent = nullptr);

    void set_points(const QVector<QPointF>& points);
    [[nodiscard]] QVector<QPointF> points() const { return points_; }
    void set_tint(const QColor& tint);
    void set_veil(const std::vector<float>& col_heights);

signals:
    void points_changed();
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
    QPointF last_plot_px_;
};

class CurvesPanel : public QWidget {
    Q_OBJECT
public:
    using CurveParams = canvas::core::colorsci::CurveParams;

    explicit CurvesPanel(QWidget* parent = nullptr);

    void set_params(const CurveParams& params);
    [[nodiscard]] CurveParams params() const;

    void set_veil(const std::vector<float>& col_heights);

signals:
    void curves_preview();
    void curves_committed(const canvas::core::colorsci::CurveParams& params);

private:
    enum Channel : int { kLuma = 0, kRed = 1, kGreen = 2, kBlue = 3 };

    void set_channel(int channel);
    void save_active_channel();
    void editor_points_changed();
    void editor_points_committed();
    void tone_changed(int field, double value);

    CurveEditor* editor_ = nullptr;
    QVector<ToneField*> tone_fields_;
    std::array<QVector<QPointF>, 4> channel_points_;
    int active_channel_ = kLuma;
    bool syncing_ = false;

    [[nodiscard]] bool interaction_log_gate();
    std::chrono::steady_clock::time_point last_interaction_log_{};
};

}
