#pragma once

// The Inspector Equalizer's frequency-response graph widget (EqGraphWidget).
//
// A hand-painted response plot: x = Hz (log), y = dB (-24..+24). The curve is
// the TRUE 6-band cascade magnitude (RBJ biquads via the shared headless law
// canvas::core::equalizer_response) — the same math the playback/export DSP
// runs, so what the plot shows is exactly what the clip filters. The band
// markers sit at each band's nominal (type/freq/gain/Q) params and are purely
// handles — the curve already reflects their real interaction.
//
// Two views, switched by the View toggle above the category:
//   Curve — the classic node graph (Resolve-style): LMB-drag a node =
//           frequency + gain (joint), Shift = frequency only, Ctrl/Alt = gain
//           only; mouse wheel over a node (or the selected band) = Q in log
//           steps; click selects (echoed by a ring + row highlight);
//           double-click toggles the band's enable. LowPass/HighPass have no
//           meaningful gain — vertical drag is refused and the node stays
//           pinned to its (locked) gain line.
//   Bands — one vertical gain fader per band arranged as a row of columns
//           (the Faders view): drag a column = that band's gain (the only
//           axis), wheel = Q, the dot atop each column toggles the band.
//
// Live band edits stream through `on_edit`; a drag release / double-click /
// wheel settle calls `on_commit` exactly once, so each gesture is one undoable
// audio-processing edit.
//
// Extracted from InspectorAudio.cpp so the graph widget can evolve (or be
// restyled for a redesign) independently of the row/commit plumbing around it.

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

// The EQ response graph used by the Inspector Audio tab. Owns the six bands it
// renders (seeded from the core defaults); the numeric rows push set_bands()
// on every edit and read bands() while the graph drags.
class EqGraphWidget final : public QWidget {
public:
    using Bands = std::array<canvas::core::Clip::EqBand, 6>;

    // Two render modes, switched by the View toggle above the category:
    //   Curve  — classic node graph on the log-frequency axis. Default.
    //   Bands  — one vertical gain fader per band, arranged as a row of
    //            columns.
    enum class View : int { Curve = 0, Bands = 1 };

    explicit EqGraphWidget(QWidget* parent = nullptr);

    void set_bands(const Bands& bands);
    const Bands& bands() const { return bands_; }
    int selected() const { return selected_; }

    View view() const { return view_; }
    void set_view(View v);

    // Selection echo → row highlight + clear-on-commit state.
    void set_selected(int index);

    // Callbacks wired by build_inspector_audio. `on_edit` fires on every live
    // band change (drag/wheel/type/enable via the graph); `on_commit` fires
    // once when a gesture settles and wants one undoable edit recorded.
    std::function<void(int index)> on_edit;
    std::function<void()> on_commit;
    std::function<void(int index)> on_selection_changed;

    // Per-band accent hues, shared by the graph nodes and the row labels so the
    // two never drift. A coherent, single-family hue set (Apple's system accent
    // colors) instead of ad-hoc pastels — stays legible per-band without
    // fighting the app's Nova-gold accent or reading as a mismatched rainbow.
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
    // The band index under `pos` (nearest node within the hit radius), or -1.
    [[nodiscard]] int node_at(const QPointF& pos) const;
    // LP/HP have no gain knob: the node is pinned to the 0 dB line and vertical
    // drag is refused (both here and via the greyed row spinbox).
    [[nodiscard]] static bool gain_locked(const canvas::core::Clip::EqBand& b);

    void emit_selection();

    Bands bands_ = canvas::core::Clip::default_eq_bands();
    View view_ = View::Curve;
    int selected_ = -1;
    int hovered_ = -1;   // band under the cursor (curve nodes only)
    int drag_band_ = -1;
    bool dragging_ = false;
    QTimer wheel_timer_;
};

}  // namespace canvas::gui