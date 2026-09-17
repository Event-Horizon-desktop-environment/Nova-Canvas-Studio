#pragma once

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

    void set_frame(canvas::core::RenderFramePtr frame);

    void refresh();

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    void recompute();
    void accumulate(const canvas::core::VideoFrame& rgba);
    void accumulate(const canvas::core::Nv12Frame& nv12);
    void render_density();

    static constexpr double kXmin = 0.10;
    static constexpr double kXmax = 0.78;
    static constexpr double kYmin = 0.00;
    static constexpr double kYmax = 0.88;
    static constexpr int kScatterGrid = 480;

    canvas::core::RenderFramePtr frame_;
    bool dirty_ = false;
    std::array<std::uint32_t, kScatterGrid * kScatterGrid> grid_{};
    QImage scatter_;

    canvas::core::gpu::ColorMatrix last_spec_matrix_ = canvas::core::gpu::ColorMatrix::BT709;
    canvas::core::gpu::ColorRange last_spec_range_ = canvas::core::gpu::ColorRange::Limited;
    bool spec_seen_ = false;
};

}
