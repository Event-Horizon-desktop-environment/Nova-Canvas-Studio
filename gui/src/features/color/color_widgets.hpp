#pragma once

#include <QColor>
#include <QImage>
#include <QVector>
#include <QWidget>

#include <array>
#include <chrono>

#include "canvas/core/media/frame.hpp"
#include "canvas/core/colorsci/wheels_ui.hpp"
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
class SwatchStrip;
class VectorscopeScope;
class WaveformScope;

enum class ToneFieldMode { kFieldOnly, kFieldReset, kFull };

enum class SwatchKind { kNone, kTemp, kTint, kHue };

class ToneField : public QWidget {
    Q_OBJECT
public:
    ToneField(const QString& label, double lo, double hi, double value = 0.0,
              double reset_value = 0.0, QWidget* parent = nullptr,
              ToneFieldMode mode = ToneFieldMode::kFull,
              SwatchKind swatch = SwatchKind::kNone);
    void set_value(double value);
    [[nodiscard]] double value() const;
    [[nodiscard]] QString label_text() const;

signals:
    void value_changed(double value);
    void reset_clicked();

protected:
    void resizeEvent(QResizeEvent* event) override;
    void showEvent(QShowEvent* event) override;

private slots:
    void slider_moved(int pos);
    void spin_changed(double value);

private:
    void sync_slider_from_spin();
    void update_swatch();
    void log_geometry(const char* tag);
    QDoubleSpinBox* spin_ = nullptr;
    QSlider* slider_ = nullptr;
    QLabel* label_ = nullptr;
    QToolButton* reset_ = nullptr;
    SwatchStrip* swatch_ = nullptr;
    double lo_ = 0.0;
    double hi_ = 1.0;
    double reset_value_ = 0.0;
    ToneFieldMode mode_ = ToneFieldMode::kFull;
    bool syncing_ = false;
};

class ColorWheelWidget : public QWidget {
    Q_OBJECT
public:
    explicit ColorWheelWidget(QWidget* parent = nullptr);

    void set_xy(const QPointF& xy);
    [[nodiscard]] QPointF xy() const { return xy_; }
    void set_active(bool on) { active_ = on; update(); }

signals:
    void xy_changed(const QPointF& xy);
    void xy_committed(const QPointF& xy);

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;

private:
    [[nodiscard]] QRectF disc_rect() const;
    [[nodiscard]] QPointF pos_to_xy(const QPointF& pos) const;
    [[nodiscard]] QPointF xy_to_pos(const QPointF& xy) const;
    void rebuild_face_cache();
    QImage face_cache_;
    QSize face_cache_size_;
    QPointF xy_ = QPointF(0.0, 0.0);
    QPointF last_xy_ = QPointF(0.0, 0.0);
    bool active_ = false;
    bool dragging_ = false;
};

class MiniKnob : public QWidget {
    Q_OBJECT
public:
    explicit MiniKnob(QWidget* parent = nullptr);

    void set_value01(float t01);
    [[nodiscard]] float value01() const { return t01_; }

signals:
    void value_changed(float t01);
    void value_committed(float t01);

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;

private:
    float t01_ = 0.5f;
    float press_t01_ = 0.5f;
    QPointF press_pos_;
    bool dragging_ = false;
};

class ColorWheelsPanel : public QWidget {
    Q_OBJECT
public:
    explicit ColorWheelsPanel(QWidget* parent = nullptr);

    void set_state(const canvas::core::colorsci::WheelPanelState& state);
    [[nodiscard]] canvas::core::colorsci::WheelPanelState state() const;

signals:
    void params_preview();
    void params_committed(const canvas::core::colorsci::WheelPanelState& state);
    void reset_all_requested();

private:
    void wheel_moved(int index, const QPointF& xy);
    void wheel_committed(int index, const QPointF& xy);
    void master_moved(int index, float t01);
    void tone_param_changed(int param, double value);
    void refresh_wheel_readout(int index);
    void commit();

    [[nodiscard]] bool is_center_release(const QPointF& xy) const;
    [[nodiscard]] std::array<float, 3> wheel_offset_for_roundtrip(int index) const;

    [[nodiscard]] bool interaction_log_gate();
    std::chrono::steady_clock::time_point last_interaction_log_{};

    QLabel* title_ = nullptr;
    QVector<ColorWheelWidget*> wheels_;
    QVector<MiniKnob*> masters_;
    QVector<QLabel*> master_values_;
    QVector<QVector<QLabel*>> channel_readouts_;
    QVector<ToneField*> tone_fields_;
    canvas::core::colorsci::WheelPanelState state_;
};


class ScopesPanel : public QWidget {
    Q_OBJECT
public:
    explicit ScopesPanel(QWidget* parent = nullptr);

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

}
