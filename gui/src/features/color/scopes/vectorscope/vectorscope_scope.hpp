#pragma once

#include <QImage>

#include <array>
#include <cstdint>

#include "features/color/scopes/common/scope_common.hpp"

#include "canvas/core/gpu/colorspace.hpp"

namespace canvas::gui {

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

    canvas::core::gpu::ColorMatrix last_spec_matrix_ = canvas::core::gpu::ColorMatrix::BT709;
    canvas::core::gpu::ColorRange last_spec_range_ = canvas::core::gpu::ColorRange::Limited;
    bool spec_seen_ = false;
};

}
