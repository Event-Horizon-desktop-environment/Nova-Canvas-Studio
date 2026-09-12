#pragma once

// Real RGB Parade scope — implements rgb-parade-scope-implementation-spec.md
// (§1–§5). Three side-by-side column-histogram waveform monitors, one per RGB
// channel, computed once per displayed frame from the SAME RenderFrame the
// preview viewer presents (SequenceController::frame_ready), so it shows the
// graded/mixed output, not the raw source (spec §5).

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

}  // namespace canvas::gui