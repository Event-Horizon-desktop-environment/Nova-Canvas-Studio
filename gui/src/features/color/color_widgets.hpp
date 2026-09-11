#pragma once

// Color-page interaction widgets (Milestone 0 UX scaffold): the Resolve-style
// wheels / curves / scopes panels, all self-contained and theme-aware. These
// only drive their own visual state for now — no grading math is claimed to be
// correct until the core grade model lands (see color.md §3.6 M0).

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

// The parameter-row unit: label + boxed numeric value. The numeric box is a
// drag-scrub field (click on it and drag up/down to adjust — no slider track),
// and an optional reset arrow returns to `reset_value`. All three controls
// write through the same `value_changed` signal.
enum class ToneFieldMode { kFieldOnly, kFieldReset, kFull };

// Color swatch beneath a field's box, painted as a thin 4px gradient strip
// with a non-interactive value marker (mockup "swatch-slot"): kNone leaves an
// empty transparent slot so all boxes on a row share one baseline.
enum class SwatchKind { kNone, kTemp, kTint, kHue };

class ToneField : public QWidget {
    Q_OBJECT
public:
    // kFieldOnly: label + boxed number (row above the wheels). kFieldReset:
    // label + boxed number + reset arrow (row below the wheels). kFull: the
    // legacy triplet with a slider track (curves Soft Clip row).
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

// Circular hue/saturation disc with a draggable position marker (the "color
// knob"). The wheel is a
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
    // Emitted when a drag ends (mouse release): the contact point where the
    // panel should draw its undo boundary. xy_changed fires on every move.
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
    // Last knob position during a drag, for the per-move delta trace.
    QPointF last_xy_ = QPointF(0.0, 0.0);
    bool active_ = false;
    bool dragging_ = false;
};

// A tiny rotary knob in the style of an old cassette player volume wheel:
// drag vertically (up = increase) or horizontally to rotate the indicator.
// Exposes a normalized value 0..1; paint draws the disc + indicator tick.
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

// Primaries panel: header (title + icon cluster) over four wheels in ONE
// always-visible 4-across row (no pagination). Each wheel column follows the
// hdr-color-wheels mockup's `wtop` anatomy: a dial-block (cassette master knob
// + value) on the left, the name centered, reset arrow on the right. The
// wheels + their master knobs + the shared tone rows bind to ONE headless
// WheelPanelState (colorsci/wheels_ui.hpp) and commit through it: every
// drag-end or tone reset emits params_committed with the full panel state,
// which the Color page turns into a set_clip_grade edit op. The panel itself
// stays a thin view — all math lives in the controller law.
class ColorWheelsPanel : public QWidget {
    Q_OBJECT
public:
    explicit ColorWheelsPanel(QWidget* parent = nullptr);

    // Load/save the controller state (used when a clip's grade round-trips).
    void set_state(const canvas::core::colorsci::WheelPanelState& state);
    [[nodiscard]] canvas::core::colorsci::WheelPanelState state() const;

signals:
    // Live wheel/master/tone movement (no undo boundary). Drives previews.
    void params_preview();
    // A drag/release/tone-commit finished: the panel state changed and the
    // Color page should commit ONE undoable set_clip_grade.
    void params_committed(const canvas::core::colorsci::WheelPanelState& state);
    // Reset-all requested: the wheels panel reverted itself to identity and
    // defers the SINGLE undo to the page, which must also clear the Curves
    // panel so the whole grade truly reverts (the wheels alone can't see the
    // curves state). The page writes an empty grade and commits once.
    void reset_all_requested();

private:
    void wheel_moved(int index, const QPointF& xy);
    void wheel_committed(int index, const QPointF& xy);
    void master_moved(int index, float t01);
    void tone_param_changed(int param, double value);
    void refresh_wheel_readout(int index);
    void commit();

    // Is a color-knob release at `xy` a "return-to-center" (revert) gesture?
    // Within a small dead zone around the disc center, releasing the color knob commits
    // identity for that wheel (same law as the per-wheel reset button).
    [[nodiscard]] bool is_center_release(const QPointF& xy) const;
    [[nodiscard]] std::array<float, 3> wheel_offset_for_roundtrip(int index) const;

    // Rate limit for the per-move interaction taps (wheel drags / master knob
    // spins / tone scrubs fire at mouse-move rate; the committed path — release,
    // reset, tone commit — always logs). Keeps the array of "touched" columns
    // readable while a multi-second drag still proves liveness.
    [[nodiscard]] bool interaction_log_gate();
    std::chrono::steady_clock::time_point last_interaction_log_{};

    QLabel* title_ = nullptr;
    QVector<ColorWheelWidget*> wheels_;
    // Per-wheel master control: a small cassette-style rotary knob (no slider
    // track). Renders the same master01 -> value law the old slider did.
    QVector<MiniKnob*> masters_;
    // Readout labels under each master knob showing its current term value.
    QVector<QLabel*> master_values_;
    // Three per-channel display boxes under each wheel (spec §9a item 2: "row
    // of exactly three small boxed numeric values").
    QVector<QVector<QLabel*>> channel_readouts_;
    QVector<ToneField*> tone_fields_;
    canvas::core::colorsci::WheelPanelState state_;
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