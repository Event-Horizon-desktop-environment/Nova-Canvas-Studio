#pragma once

// CIE 1931 chromaticity diagram — implements waveform-vectorscope-histogram-cie-
// implementation-spec.md §4. Structurally different from the other scopes: it is
// NOT a live per-frame trace but a static reference diagram (spectral locus,
// gamut triangles, Planckian locus, white point) with the current frame's pixel
// chromaticities plotted as scattered points on top. Per-pixel RGB→XYZ→xy is the
// most expensive conversion of the five scopes, so it recomputes ON DEMAND
// (panel opened / frame changed while visible / explicit refresh) rather than on
// every presented frame.

#include <QImage>
#include <QWidget>

#include <array>
#include <cstdint>

#include "canvas/core/media/frame.hpp"

#include "canvas/core/gpu/colorspace.hpp"

class QPainter;

namespace canvas::gui {

class ChromaticityWidget final : public QWidget {
public:
    explicit ChromaticityWidget(QWidget* parent = nullptr);

    // Stash the newest presented frame; the scatter recomputes lazily on the
    // next paint only if the frame actually changed (spec §4 lazy cadence).
    void set_frame(canvas::core::RenderFramePtr frame);

    // Force a recompute now (panel opened / explicit refresh).
    void refresh();

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    void recompute();
    void accumulate(const canvas::core::VideoFrame& rgba);
    void accumulate(const canvas::core::Nv12Frame& nv12);
    void render_density();

    // xy-domain of the diagram (square-projected so shapes keep their truth).
    static constexpr double kXmin = 0.10;
    static constexpr double kXmax = 0.78;
    static constexpr double kYmin = 0.00;
    static constexpr double kYmax = 0.88;
    static constexpr int kScatterGrid = 480;

    canvas::core::RenderFramePtr frame_;
    bool dirty_ = false;
    std::array<std::uint32_t, kScatterGrid * kScatterGrid> grid_{};
    QImage scatter_;

    // Last Nv12Frame spec this scope analysed, so the color archive logs a
    // [scope] spec-change line exactly once per source switch.
    canvas::core::gpu::ColorMatrix last_spec_matrix_ = canvas::core::gpu::ColorMatrix::BT709;
    canvas::core::gpu::ColorRange last_spec_range_ = canvas::core::gpu::ColorRange::Limited;
    bool spec_seen_ = false;
};

}  // namespace canvas::gui