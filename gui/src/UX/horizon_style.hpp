#pragma once

#include <QProxyStyle>

class QPainter;
class QStyleOption;

namespace canvas::gui {

// Horizon-style QStyle built on Fusion that paints flat studio-console panels
// and buttons via QPainter — a plain fill with a single hairline rim, modest
// fixed radii, and command buttons as accent fills rather than capsules. The
// base Fusion style handles everything this treatment does not (menus,
// scrollbars, views, text layout, etc.).
class HorizonStyle final : public QProxyStyle {
public:
    explicit HorizonStyle(QStyle* base);

    void drawPrimitive(PrimitiveElement pe, const QStyleOption* opt, QPainter* p,
                       const QWidget* w) const override;
    void drawControl(ControlElement ce, const QStyleOption* opt, QPainter* p,
                     const QWidget* w) const override;
    int pixelMetric(PixelMetric pm, const QStyleOption* option, const QWidget* widget) const override;
};

}  // namespace canvas::gui
