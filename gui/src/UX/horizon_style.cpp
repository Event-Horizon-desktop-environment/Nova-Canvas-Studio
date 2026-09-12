#include "UX/horizon_style.hpp"

#include "UX/theme.hpp"

#include <QPainter>
#include <QPainterPath>
#include <QStyleOption>

namespace canvas::gui {

namespace {

// Button corner radius for everything drawn by this style. There is no capsule
// in the flat-studio language: command/accent buttons are a plain accent fill,
// never a pill (buttons.md — keep the prominent style to one action per view,
// and pills everywhere read as decoration).
constexpr qreal kButtonRadius = 8.0;

// Paint a flat surface: a plain fill with a single hairline rim. No sheen
// gradient — that top-lit highlight and its specular catch-light were the
// Liquid-Glass idiom and are gone. All colours come from the active
// ThemeTokens so the style follows a mode switch.
void paintFlatSurface(QPainter* p, const QRectF& r, qreal radius, const QColor& fill,
                      bool hovered, bool pressed, bool accent, bool checked) {
    const ThemeTokens& t = tokens();
    p->setRenderHint(QPainter::Antialiasing, true);

    const qreal rad = radius;
    QRectF body = r.adjusted(0.5, 0.5, -0.5, -0.5);
    if (body.isEmpty())
        return;

    QColor base = fill;
    if (accent) {
        base = (pressed | checked) ? t.accent_press : (hovered ? t.accent_hover : t.accent);
    } else if (pressed) {
        base = t.border;
    } else if (hovered) {
        base = t.surface_higher;
    }

    QPainterPath clip;
    clip.addRoundedRect(body, rad, rad);
    p->setPen(QPen(accent ? t.accent_press : t.border, 1.0));
    p->setBrush(base);
    p->drawPath(clip);
}

}  // namespace

HorizonStyle::HorizonStyle(QStyle* base) : QProxyStyle(base) {}

void HorizonStyle::drawPrimitive(PrimitiveElement pe, const QStyleOption* opt, QPainter* p,
                                 const QWidget* w) const {
    switch (pe) {
        case PE_PanelButtonCommand:
        case PE_PanelButtonTool: {
            // Command (push) buttons render as a flat amber accent fill; tool
            // buttons keep the plain raised surface. Both share the same modest
            // radius — no capsules anywhere.
            const bool is_cmd = (pe == PE_PanelButtonCommand);
            bool hover = opt->state & State_MouseOver;
            bool down = opt->state & State_Sunken && opt->state & State_Enabled;
            bool checked = opt->state & State_On;
            paintFlatSurface(p, QRectF(opt->rect), kButtonRadius,
                             tokens().surface_raised, hover, down, is_cmd, checked);
            return;
        }
        case PE_PanelMenuBar:
        case PE_PanelToolBar:
        case PE_PanelTipLabel: {
            // Full-bleed chrome strips — flush, straight ends, no card.
            paintFlatSurface(p, QRectF(opt->rect), 0, tokens().surface_raised,
                             false, false, false, false);
            return;
        }
        case PE_FrameGroupBox: {
            QRectF r = opt->rect;
            p->setPen(QPen(tokens().border, 1.0));
            p->setBrush(Qt::NoBrush);
            p->setRenderHint(QPainter::Antialiasing, true);
            p->drawRoundedRect(r.adjusted(0.5, 0.5, -0.5, -0.5), 10, 10);
            return;
        }
        default:
            break;
    }
    QProxyStyle::drawPrimitive(pe, opt, p, w);
}

void HorizonStyle::drawControl(ControlElement ce, const QStyleOption* opt, QPainter* p,
                               const QWidget* w) const {
    switch (ce) {
        case CE_ToolBar: {
            const auto* o = qstyleoption_cast<const QStyleOptionToolBar*>(opt);
            QColor fill = tokens().surface_raised;
            if (o && (o->toolBarArea == Qt::BottomToolBarArea)) {
                // Bottom page bar reads as a recessed full-width well: square ends.
                fill = tokens().surface;
            }
            paintFlatSurface(p, QRectF(opt->rect), 0, fill, false, false, false, false);
            return;
        }
        case CE_PushButton:
        case CE_PushButtonBevel: {
            // Flat bevel drawn in drawPrimitive(PE_PanelButtonCommand); let
            // Fusion handle label + icon on top of it.
            break;
        }
        default:
            break;
    }
    QProxyStyle::drawControl(ce, opt, p, w);
}

int HorizonStyle::pixelMetric(PixelMetric pm, const QStyleOption* option, const QWidget* widget) const {
    if (pm == PM_ButtonMargin) return 8;
    if (pm == PM_MenuBarItemSpacing) return 2;
    return QProxyStyle::pixelMetric(pm, option, widget);
}

}  // namespace canvas::gui