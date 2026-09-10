#pragma once

// Vectorscope — implements waveform-vectorscope-histogram-cie-implementation-
// spec.md §2. A 2D polar scatter of the chrominance plane: every sampled pixel
// is converted R'G'B' → Y'CbCr with the same Rec.601 matrix the decode pipeline
// uses, then accumulated into a Cb×Cr bucket grid. The fixed graticule is a
// broadcast-compass layout (Tektronix-derived): outer full-swing circle + 10°
// degree ticks with 30° numerals, ±U/±V axes, NTSC I/Q diameters with the
// amber +I skin-tone line, a dashed 75% calibration ring, and six 75% color-bar
// box targets. The optional 2x is a trace-gain zoom (the trace scales, the
// graticule stays fixed on screen).

#include <QImage>

#include <array>
#include <cstdint>

#include "features/color/scopes/common/scope_common.hpp"

#include "canvas/core/gpu/colorspace.hpp"

namespace canvas::gui {

// How the chrominance cloud is shaded (panel "Trace" sub-dropdown; the item
// order and these values are wired by index, Color is the default).
//   Color — true-color: each cell is the average R'G'B' of its pixels, with the
//           dense core burning out to white (DaVinci/Resolve-style)
//   Mono  — pure density: white cloud, best for legal-limit / channel readings
//   Green — classic broadcast phosphor tint
enum class TraceMode { Color, Mono, Green };

class VectorscopeScope final : public ScopePane {
public:
    using ScopePane::ScopePane;

    void set_gain_percent(int percent) {
        gain_percent_ = std::clamp(percent, 10, 500);
        render_density();
        update();
    }
    void set_zoom2x(bool on) {
        zoom2x_ = on;
        update();
    }
    void set_trace_mode(TraceMode mode) {
        trace_mode_ = mode;
        render_density();
        update();
    }

protected:
    void recompute_render() override;
    void paint_body(QPainter& p, const QRectF& plot) override;

private:
    // One cell per Cb×Cr bucket: pixel count + summed R'G'B' channels so the
    // true-color cloud can render each bucket at its average color (256²-cell
    // sums cap ~66M, comfortably inside uint64).
    struct ScatterCell {
        std::uint32_t count = 0;
        std::uint64_t r_sum = 0;
        std::uint64_t g_sum = 0;
        std::uint64_t b_sum = 0;
    };

    void accumulate(const canvas::core::VideoFrame& rgba);
    void accumulate(const canvas::core::Nv12Frame& nv12);
    void render_density();
    void paint_graticule(QPainter& p, const QRectF& plot) const;
std::array<ScatterCell, kScopeVec * kScopeVec> scatter_{};
    int gain_percent_ = 100;
    bool zoom2x_ = false;
    TraceMode trace_mode_ = TraceMode::Color;
    QImage content_;

    // Last Nv12Frame spec this scope accumulated, so the color archive logs a
    // [scope] spec-change line exactly once per source switch (not 60/s).
    canvas::core::gpu::ColorMatrix last_spec_matrix_ = canvas::core::gpu::ColorMatrix::BT709;
    canvas::core::gpu::ColorRange last_spec_range_ = canvas::core::gpu::ColorRange::Limited;
    bool spec_seen_ = false;
};

}  // namespace canvas::gui