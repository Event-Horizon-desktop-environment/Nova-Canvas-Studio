#pragma once

#include <QTimer>
#include <QWidget>

#include <array>
#include <functional>

#include "canvas/core/timeline/model.hpp"

class QColor;
class QEvent;
class QMouseEvent;
class QPainter;
class QPaintEvent;
class QPointF;
class QRectF;
class QWheelEvent;

namespace canvas::gui {

class EqGraphWidget final : public QWidget {
public:
    using Bands = std::array<canvas::core::Clip::EqBand, 6>;

    enum class View : int { Curve = 0, Bands = 1 };

    explicit EqGraphWidget(QWidget* parent = nullptr);

    void set_bands(const Bands& bands);
    const Bands& bands() const { return bands_; }
    int selected() const { return selected_; }

    View view() const { return view_; }
    void set_view(View v);

    void set_selected(int index);

    std::function<void(int index)> on_edit;
    std::function<void()> on_commit;
    std::function<void(int index)> on_selection_changed;

    static QColor band_hue(int index);

protected:
    void paintEvent(QPaintEvent*) override;
    void paint_bands(QPainter& p, const QRectF& r,
                     const std::function<double(double)>& y_for);
    [[nodiscard]] int column_at(double px) const;
    void mousePressEvent(QMouseEvent* e) override;
    void mouseMoveEvent(QMouseEvent* e) override;
    void mouseReleaseEvent(QMouseEvent* e) override;
    void leaveEvent(QEvent*) override;
    void mouseDoubleClickEvent(QMouseEvent* e) override;
    void wheelEvent(QWheelEvent* e) override;

private:
    [[nodiscard]] static constexpr double kMinHzV() { return 20.0; }
    [[nodiscard]] static constexpr double kMaxHzV() { return 20000.0; }
    [[nodiscard]] static constexpr double kMinDbV() { return -24.0; }
    [[nodiscard]] static constexpr double kMaxDbV() { return 24.0; }

    [[nodiscard]] QRectF plot_rect() const;
    [[nodiscard]] int node_at(const QPointF& pos) const;
    [[nodiscard]] static bool gain_locked(const canvas::core::Clip::EqBand& b);

    void emit_selection();

    Bands bands_ = canvas::core::Clip::default_eq_bands();
    View view_ = View::Curve;
    int selected_ = -1;
    int hovered_ = -1;
    int drag_band_ = -1;
    bool dragging_ = false;
    QTimer wheel_timer_;
};

}
