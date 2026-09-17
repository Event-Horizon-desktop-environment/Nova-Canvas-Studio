#pragma once

#include <QImage>

#include "features/color/scopes/common/scope_common.hpp"

namespace canvas::gui {

class WaveformScope final : public ScopePane {
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
    ScopeDisplay display_ = ScopeDisplay::Luma;
    QImage content_;
};

}
