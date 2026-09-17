#pragma once

#include <QImage>

#include <array>
#include <cstdint>

#include "features/color/scopes/common/scope_common.hpp"

#include "canvas/core/gpu/colorspace.hpp"

namespace canvas::gui {

class HistogramScope final : public ScopePane {
public:
    using ScopePane::ScopePane;

    void set_display(ScopeDisplay display) {
        display_ = display;
        render_density();
        update();
    }
    [[nodiscard]] ScopeDisplay display() const { return display_; }

protected:
    void recompute_render() override;
    void paint_body(QPainter& p, const QRectF& plot) override;

private:
    void render_density();

    ColumnHistogram hist_;
    ScopeDisplay display_ = ScopeDisplay::Rgb;
    std::array<std::uint32_t, 3 * kScopeLevels> hist1d_{};
    std::array<std::uint32_t, kScopeLevels> hist1d_luma_{};
    QImage content_;

    canvas::core::gpu::ColorMatrix last_spec_matrix_ = canvas::core::gpu::ColorMatrix::BT709;
    canvas::core::gpu::ColorRange last_spec_range_ = canvas::core::gpu::ColorRange::Limited;
    bool spec_seen_ = false;
};

}
