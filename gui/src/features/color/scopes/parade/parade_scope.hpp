#pragma once

#include <QImage>

#include "features/color/scopes/common/scope_common.hpp"

namespace canvas::gui {

class ParadeScope final : public ScopePane {
public:
    using ScopePane::ScopePane;

protected:
    void recompute_render() override;
    void paint_body(QPainter& p, const QRectF& plot) override;

private:
    ColumnHistogram hist_;
    QImage content_;
};

}
