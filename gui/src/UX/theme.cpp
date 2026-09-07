#include "UX/theme.hpp"

#include "UX/horizon_style.hpp"

#include <QApplication>
#include <QColor>
#include <QIconEngine>
#include <QPainter>
#include <QPalette>
#include <QPixmap>
#include <QStyleFactory>
#include <QSvgRenderer>

#include <utility>

namespace canvas::gui {

namespace {

// ---------------------------------------------------------------------------
// Horizon design tokens — the dark, slightly blue-tinted palette shared by the
// Event-Horizon apps, with the Nova-Canvas-Studio blue kept as the accent.
// ---------------------------------------------------------------------------
constexpr const char* kSurface        = "#11131A";  // window / deepest base
constexpr const char* kSurfaceLow     = "#141A21";  // recessed wells
constexpr const char* kSurfaceRaised  = "#1A1D27";  // panels, cards, buttons
constexpr const char* kSurfaceHigher  = "#20242F";  // hovered raised surfaces
constexpr const char* kSurfaceHighest = "#272C39";  // menus / popups
constexpr const char* kBorder         = "#2A2F3C";  // strong separators
constexpr const char* kBorderSoft     = "#232833";  // hairlines

constexpr const char* kInk        = "#E8EAF0";  // primary text
constexpr const char* kInkMuted   = "#9AA0B0";  // secondary text
constexpr const char* kInkFaint   = "#5F6577";  // tertiary text

constexpr const char* kPrimary       = "#3B82F6";  // blue accent (studio)
constexpr const char* kPrimaryHover  = "#4C92FF";
constexpr const char* kPrimaryPress  = "#2F6FED";
constexpr const char* kOnAccent      = "#FFFFFF";  // text on accent fills
constexpr const char* kAccentSoft    = "rgba(59,130,246,0.16)";  // translucent accent fill
constexpr const char* kAccentText    = "#A6C7FF";  // accent-colored text on surfaces

constexpr const char* kError   = "#F0717A";
constexpr const char* kErrorSoft = "rgba(240,113,122,0.15)";

// State-layer fills (Material-style overlays).
constexpr const char* kStateHover    = "rgba(255,255,255,0.08)";
constexpr const char* kStatePress    = "rgba(255,255,255,0.12)";
constexpr const char* kStateSelected = "rgba(59,130,246,0.20)";

constexpr const char* kFocusRing = "#A6C7FF";

// Shape (Horizon / M3 shape scale).
constexpr int kShapeSmall  = 8;
constexpr int kShapeMedium = 12;
constexpr int kShapeLarge  = 16;
constexpr int kShapePill   = 999;  // pill (radius = height/2 at draw time)

// Tints for the bundled monochrome SVG icons, mapped per QIcon mode.
class SvgIconEngine : public QIconEngine {
public:
    explicit SvgIconEngine(QString file, QColor normal = QColor(), bool tint = true)
        : file_(std::move(file)), normal_(std::move(normal)), tint_(tint) {}

    QIconEngine* clone() const override { return new SvgIconEngine(file_, normal_); }

    void paint(QPainter* painter, const QRect& rect, QIcon::Mode mode, QIcon::State state) override {
        painter->drawPixmap(rect, pixmap(rect.size(), mode, state));
    }

    QPixmap pixmap(const QSize& size, QIcon::Mode mode, QIcon::State) override {
        if (size.isEmpty())
            return QPixmap();  // never paint into a null pixmap (engine == 0)
        int dpr = 1;
        if (QApplication* app = qobject_cast<QApplication*>(QCoreApplication::instance())) {
            dpr = int(app->devicePixelRatio());
        }
        const QSize px(size.width() * dpr, size.height() * dpr);

        // Render at 2x the target (supersample) and smoothly downscale, so
        // both vector strokes and any down-converted raster art inside the SVG
        // (e.g. the film-strip thumbnail) stay crisp instead of pixellated.
        constexpr int kSupersample = 2;
        const QSize big(px.width() * kSupersample, px.height() * kSupersample);
        QPixmap hi(big);
        hi.fill(Qt::transparent);
        {
            QSvgRenderer renderer(QStringLiteral(":/icons/%1.svg").arg(file_));
            QPainter p(&hi);
            p.setRenderHint(QPainter::Antialiasing);
            p.setRenderHint(QPainter::SmoothPixmapTransform);
            renderer.render(&p, QRectF(QPointF(0, 0), QSizeF(big)));
            p.end();
        }

        QPixmap pm(px);
        pm.fill(Qt::transparent);
        pm.setDevicePixelRatio(dpr);
        {
            QPainter d(&pm);
            d.setRenderHint(QPainter::SmoothPixmapTransform);
            d.drawPixmap(QRectF(QPointF(0, 0), QSizeF(px)), hi,
                         QRectF(QPointF(0, 0), QSizeF(big)));
            d.end();
        }
        QColor tint = modeColor(mode);
        if (normal_.isValid() && (mode == QIcon::Normal || mode == QIcon::Selected)) {
            tint = normal_;
        }
        if (tint_ && tint.isValid() && tint != QColor(Qt::transparent)) {
            QPixmap colored(px);
            colored.fill(tint);
            colored.setDevicePixelRatio(dpr);
            QPainter tp(&colored);
            tp.setCompositionMode(QPainter::CompositionMode_SourceIn);
            tp.drawPixmap(0, 0, pm);
            tp.end();
            pm = colored;
        }
        return pm;
    }

private:
    static QColor modeColor(QIcon::Mode mode) {
        switch (mode) {
            case QIcon::Normal:
            case QIcon::Selected:
                return QColor(0x9A, 0xA0, 0xB0);  // muted ink, icon idle
            case QIcon::Active:
                return QColor(kInk);              // brighten on hover/active
            case QIcon::Disabled:
                return QColor(0x5F, 0x65, 0x77);  // faint ink, disabled
        }
        return QColor(Qt::transparent);
    }

    QColor normal_;
    QString file_;
    bool tint_;
};

// Global stylesheet for the flat controls only. Buttons, tool bars and panels
// are intentionally left OUT so HorizonStyle's glassy painting owns them.
QString make_flat_controls_qss() {
    return QStringLiteral(
      "QWidget { color: %1; }"
      "QToolTip { background-color: %2; color: %1; border: 1px solid %3;"
      " padding: 4px 8px; border-radius: 8px; }"
      "QMenuBar { background: transparent; color: %1; }"
      "QMenuBar::item { background: transparent; border-radius: 8px; padding: 4px 10px; }"
      "QMenuBar::item:selected { background: %4; }"
      "QMenuBar::item:pressed { background: rgba(255,255,255,0.12); }"
      // Floating menu card: one raised semi-rounded popup surface matching the
      // app's card language, with pill-shaped hover items and a blue check
      // indicator for checkable actions (right-click / bar menus alike).
      "QMenu { background: #20242F; color: #E8EAF0; border: 1px solid #2A2F3C;"
      "  border-radius: 12px; padding: 6px; }"
      "QMenu::item { padding: 5px 24px 5px 12px; border-radius: 8px; margin: 1px 4px; }"
      "QMenu::item:selected { background: rgba(255,255,255,0.08); color: #FFFFFF; }"
      "QMenu::item:disabled { color: #5F6577; }"
      "QMenu::separator { height: 1px; background: #2A2F3C; margin: 5px 8px; }"
      "QMenu::indicator { width: 16px; height: 16px; margin: 0 4px; }"
      "QMenu::indicator:checked { background: #3B82F6; border-radius: 4px;"
      "  image: url(:/icons/check.svg); }"
      "QMenu::icon { margin-left: 4px; margin-right: 8px; }"
      "QMenu::separator:horizontal { height: 1px; }"
      "QStatusBar { color: %7; }"
      "QTabBar::tab { background: transparent; color: %7; padding: 6px 14px;"
      "  border-bottom: 2px solid transparent; }"
      "QTabBar::tab:selected { color: %8; border-bottom: 2px solid %9; }"
      "QTabBar::tab:hover { color: %1; }"
      "QTabBar::scroller { width: 32px; }"
      "QTabBar QToolButton { min-width: 18px; min-height: 18px; max-width: 22px;"
      "  max-height: 22px; border-radius: 9px; background: transparent; }"
      "QTabBar QToolButton:hover { background: rgba(255,255,255,0.08); }"
      "QTabBar QToolButton:pressed { background: rgba(255,255,255,0.12); }"
      "QScrollArea { background: transparent; border: none; }"
      "QScrollBar:vertical { background: transparent; width: 10px; margin: 0; }"
      "QScrollBar::handle:vertical { background: %10; border-radius: 5px; min-height: 24px; }"
      "QScrollBar::handle:vertical:hover { background: %11; }"
      "QScrollBar:horizontal { background: transparent; height: 10px; margin: 0; }"
      "QScrollBar::handle:horizontal { background: %10; border-radius: 5px; min-width: 24px; }"
      "QScrollBar::handle:horizontal:hover { background: %11; }"
      "QScrollBar::add-line, QScrollBar::sub-line { height: 0; width: 0; }"
      "QScrollBar::add-page, QScrollBar::sub-page { background: transparent; }"
      "QSlider::groove:horizontal { height: 3px; background: %10; border-radius: 1.5px; }"
      "QSlider::handle:horizontal { width: 14px; margin: -5px 0; background: %9;"
      "  border: none; border-radius: 7px; }"
      "QSlider::handle:horizontal:hover { background: %12; }"
      "QSlider::handle:horizontal:pressed { background: %9; }"
      "QSlider::sub-page:horizontal { background: %12; border-radius: 1.5px; }"
      "QCheckBox, QRadioButton { spacing: 8px; outline: none; }"
      "QCheckBox::indicator, QRadioButton::indicator { width: 16px; height: 16px;"
      "  background: %10; border: 1px solid %3; border-radius: 5px; }"
      "QCheckBox::indicator:hover, QRadioButton::indicator:hover { border-color: %12; }"
      "QCheckBox::indicator:checked { background: %13; border-color: %13;"
      "  image: url(:/icons/check.svg); }"
      "QComboBox { background: %10; color: %1; border: 1px solid %3;"
      "  border-radius: 8px; padding: 4px 10px; }"
      "QComboBox:hover { background: %11; }"
      "QComboBox QAbstractItemView { background: %2; border: none;"
      "  selection-background-color: %13; selection-color: %14; outline: none;"
      "  border-radius: 8px; padding: 4px; }"
      "QLineEdit { background: %10; color: %1; border: 1px solid %3;"
      "  border-radius: 8px; padding: 3px 8px; }"
      "QLineEdit:focus { border-color: %12; }"
      "QAbstractScrollArea::corner { background: transparent; border: none; }"
      "QDoubleSpinBox, QSpinBox { background: %10; color: %1; border: 1px solid %3;"
      "  border-radius: 8px; padding: 3px 8px; }"
      "QDoubleSpinBox:focus, QSpinBox:focus, QDoubleSpinBox:hover, QSpinBox:hover"
      "  { border-color: %12; }"
      "QDoubleSpinBox::up-button, QSpinBox::up-button,"
      "QDoubleSpinBox::down-button, QSpinBox::down-button { width: 16px;"
      "  background: transparent; border: none; margin: 1px; }"
    )
      .arg(kInk, kSurfaceHighest, kBorder,
           kStateHover, kStateSelected, kInkFaint,
           kInkMuted, kAccentText,
           kPrimary, kSurfaceHigher, kSurfaceHighest,
           kPrimaryHover, kPrimary, kOnAccent);
}

}  // namespace

// ---------------------------------------------------------------------------
// Horizon-based QPalette. Blue accent maps onto Highlight/Accent; surfaces are
// the dark blue-tinted Horizon family.
// ---------------------------------------------------------------------------
QPalette makeHorizonPalette() {
    QPalette p;
    p.setColor(QPalette::Window,        QColor(kSurface));
    p.setColor(QPalette::WindowText,    QColor(kInk));
    p.setColor(QPalette::Base,          QColor(kSurfaceLow));
    p.setColor(QPalette::AlternateBase, QColor(kSurfaceRaised));
    p.setColor(QPalette::Text,          QColor(kInk));
    p.setColor(QPalette::Button,        QColor(kSurfaceRaised));
    p.setColor(QPalette::ButtonText,    QColor(kInk));
    p.setColor(QPalette::BrightText,    QColor(kError));
    p.setColor(QPalette::Highlight,     QColor(kPrimary));
    p.setColor(QPalette::HighlightedText, QColor(kOnAccent));
    p.setColor(QPalette::PlaceholderText, QColor(kInkFaint));
    p.setColor(QPalette::ToolTipBase,   QColor(kSurfaceHighest));
    p.setColor(QPalette::ToolTipText,   QColor(kInk));
    p.setColor(QPalette::Link,          QColor(kAccentText));
    p.setColor(QPalette::Disabled, QPalette::Text,       QColor(kInkFaint));
    p.setColor(QPalette::Disabled, QPalette::WindowText, QColor(kInkFaint));
    p.setColor(QPalette::Disabled, QPalette::ButtonText, QColor(kInkFaint));
    p.setColor(QPalette::Disabled, QPalette::Highlight,  QColor(kSurfaceHigher));
    p.setColor(QPalette::Disabled, QPalette::HighlightedText, QColor(kInkFaint));
    return p;
}

void apply_theme(QApplication& app) {
    QStyle* fusion = QStyleFactory::create(QStringLiteral("Fusion"));
    app.setStyle(new HorizonStyle(fusion));
    app.setPalette(makeHorizonPalette());
    app.setStyleSheet(make_flat_controls_qss());
}

// ---------------------------------------------------------------------------
// App-specific chrome. These per-widget stylesheets layer Horizon variants on
// top of the global sheet and HorizonStyle, applied to individual widgets.
// ---------------------------------------------------------------------------
constexpr const char* kTransportBarQss = "background-color: #11131A; border-top: 1px solid #232833;";
constexpr const char* kTimelineToolsQss = "background-color: transparent;";
constexpr const char* kPageSwitcherQss = "background-color: #11131A; border-top: 1px solid #232833;";
constexpr const char* kTimeLabelQss = "font-family: monospace; color: #E8EAF0;";
constexpr const char* kMediaPoolQss =
    "QListWidget { background-color: #141A21; border: none; }"
    "QListWidget::item { background: transparent; color: #E8EAF0; padding: 5px 8px;"
    "  border-radius: 8px; margin: 1px 4px; }"
    "QListWidget::item:hover { background-color: rgba(255,255,255,0.08); }"
    "QListWidget::item:selected { background-color: #3B82F6; color: #FFFFFF; }";

constexpr const char* kViewerFrameQss =
    "QFrame#viewerFrame, QFrame#timelineFrame { background-color: #141A21;"
    " border: 1px solid #232833; border-radius: 12px; }";

constexpr const char* kTimelineFrameQss =
    "QFrame#timelineFrame { background-color: #11131A; border: 1px solid #232833;"
    " border-radius: 12px; }";

constexpr const char* kTimelineDockTitleQss =
    "QWidget#timelineDockTitle { background-color: #141A21; }";

constexpr const char* kGlobalToolbarQss = "background-color: #11131A;";

constexpr const char* kTopStatusBarQss = "background-color: #11131A; border-bottom: 1px solid #232833;";

constexpr const char* kBigTimecodeQss =
    "font-family: monospace; font-size: 15px; color: #F0F2F7; background: transparent;";

constexpr const char* kBinTreeQss =
    "QTreeWidget { background-color: #11131A; border: none; color: #E8EAF0; }"
    "QTreeWidget::branch { background: transparent; }"
    "QTreeWidget::item { padding: 3px 2px; border-radius: 8px; }"
    "QTreeWidget::item:hover { background-color: rgba(255,255,255,0.08); }"
    "QTreeWidget::item:selected { background-color: rgba(255,255,255,0.14); color: #FFFFFF; }"
    "QTreeWidget::item:selected:hover { background-color: rgba(255,255,255,0.20); }";

// Semi-rounded inspector category card: the header is the raised top band (or
// the whole card when collapsed), the body the inset content well. Radii match
// the card shape; the seam between the two is the header's bottom hairline.
constexpr const char* kInspectorCategoryHeaderQss =
    "QToolButton { background: transparent; border: none;"
    " padding: 8px 10px; text-align: left; color: #E8EAF0; font-weight: 600;"
    " border-radius: 8px; }"
    "QToolButton:hover { background-color: rgba(255,255,255,0.08); }";

// Header row (the card's top band). Two shapes: OPEN rounds the top corners
// (the body below rounds the bottom); CLOSED rounds all four corners. Scoped
// by objectName so the border never cascades onto the child toggle/reset.
constexpr const char* kInspectorCardHeaderOpenQss =
    "QWidget#inspectorCardHeader { background-color: #1A1D27;"
    " border: 1px solid #232833;"
    " border-top-left-radius: 12px; border-top-right-radius: 12px; }";
constexpr const char* kInspectorCardHeaderClosedQss =
    "QWidget#inspectorCardHeader { background-color: #1A1D27;"
    " border: 1px solid #232833; border-radius: 12px; }";
constexpr const char* kInspectorCardBodyQss =
    "QWidget#inspectorCardBody { background-color: #141A21;"
    " border-bottom-left-radius: 12px; border-bottom-right-radius: 12px;"
    " border-left: 1px solid #232833; border-right: 1px solid #232833;"
    " border-bottom: 1px solid #232833; }";

constexpr const char* kPagePillQss =
    // Raised pill with a soft border — visible but not loud.  Selected glows
    // with the accent so the active page is immediately readable.
    "QToolButton { color: #9AA0B0; padding: 5px 10px; border-radius: 8px;"
    " background-color: #1A1D27; border: 1px solid #232833; }"
    "QToolButton:checked { color: #FFFFFF; background-color: rgba(59,130,246,0.16);"
    " border: 1px solid rgba(59,130,246,0.50); font-weight: 600; }"
    "QToolButton:hover:!checked { color: #F0F2F7; background-color: #20242F;"
    " border-color: #2A2F3C; }"
    "QToolButton:pressed { background-color: #2A2F3C; }";

// Inspector mode tabs: a recessed semi-rounded segmented track (the pill row's
// container) with individual pills that read as one segmented control.
constexpr const char* kInspectorTabTrackQss =
    "QWidget { background-color: #0E1117;"
    " border: 1px solid #232833; border-radius: 12px; }";

constexpr const char* kInspectorTabQss =
    "QToolButton { color: #9AA0B0; background: transparent; border: none;"
    " border-radius: 9px; padding: 5px 4px; font-weight: 500; }"
    "QToolButton:checked { color: #FFFFFF; background-color: #3B82F6; font-weight: 600; }"
    "QToolButton:hover:!checked { color: #F0F2F7; background-color: rgba(255,255,255,0.08); }";

// Flat icon toolbar buttons — every icon button now has a subtle raised surface
// so the whole chrome feels physical, not invisible-until-hovered.
constexpr const char* kFlatToolQss =
    "QToolButton { background: #1A1D27; border: 1px solid #232833; border-radius: 6px;"
    " padding: 4px; }"
    "QToolButton:hover { background: #20242F; border-color: #2A2F3C; }"
    "QToolButton:pressed { background: #2A2F3C; }"
    "QToolButton:checked { background: rgba(59,130,246,0.20);"
    " border: 1px solid rgba(59,130,246,0.40); }";

// "Bracket-pill" edit-tool cluster (select/trim/blade/mode): raised members
// with a cohesive border, active member shows a soft blue fill.
constexpr const char* kToolClusterQss =
    "QToolButton { background: #1A1D27; border: 1px solid #232833; border-radius: 6px;"
    " padding: 4px; }"
    "QToolButton:hover { background: #20242F; border-color: #2A2F3C; }"
    "QToolButton:checked { background: rgba(59,130,246,0.20);"
    " border: 1px solid rgba(59,130,246,0.40); border-radius: 6px; }";

// Semi-rounded outlined button (DIM) — raised surface with a colored border.
constexpr const char* kOutlinePillQss =
    "QToolButton { color: #C7CCD8; background: #1A1D27; border: 1px solid #3A4150;"
    " border-radius: 8px; padding: 3px 12px; font-size: 10px; }"
    "QToolButton:hover { border-color: #5A6375; color: #FFFFFF; background: #20242F; }"
    "QToolButton:checked { border-color: #EF4444; color: #FCA5A5; background: rgba(239,68,68,0.18); }";

// Semi-rounded sliders: pill groove, round handle — never square.
constexpr const char* kSliderQss =
    "QSlider::groove:horizontal { height: 4px; background: #2A2F3C; border-radius: 2px; }"
    "QSlider::sub-page:horizontal { background: #3B82F6; border-radius: 2px; }"
    "QSlider::handle:horizontal { width: 16px; height: 16px; margin: -6px 0; background: #E8EAF0;"
    " border: none; border-radius: 8px; }"
    "QSlider::handle:horizontal:hover { background: #FFFFFF; }";

QString transport_bar_style() { return QString::fromLatin1(kTransportBarQss); }
QString timeline_tools_style() { return QString::fromLatin1(kTimelineToolsQss); }
QString page_switcher_style() { return QString::fromLatin1(kPageSwitcherQss); }
QString time_label_style() { return QString::fromLatin1(kTimeLabelQss); }
QString media_pool_style() { return QString::fromLatin1(kMediaPoolQss); }
QString viewer_frame_style() { return QString::fromLatin1(kViewerFrameQss); }
QString timeline_frame_style() { return QString::fromLatin1(kTimelineFrameQss); }
QString global_toolbar_style() { return QString::fromLatin1(kGlobalToolbarQss); }
QString top_status_bar_style() { return QString::fromLatin1(kTopStatusBarQss); }
QString big_timecode_style() { return QString::fromLatin1(kBigTimecodeQss); }
QString bin_tree_style() { return QString::fromLatin1(kBinTreeQss); }
QString inspector_category_header_style() { return QString::fromLatin1(kInspectorCategoryHeaderQss); }
QString inspector_card_header_open_style() { return QString::fromLatin1(kInspectorCardHeaderOpenQss); }
QString inspector_card_header_closed_style() { return QString::fromLatin1(kInspectorCardHeaderClosedQss); }
QString inspector_card_body_style() { return QString::fromLatin1(kInspectorCardBodyQss); }
QString inspector_tab_track_style() { return QString::fromLatin1(kInspectorTabTrackQss); }
QString inspector_tab_style() { return QString::fromLatin1(kInspectorTabQss); }
QString page_pill_style() { return QString::fromLatin1(kPagePillQss); }
QString flat_tool_style() { return QString::fromLatin1(kFlatToolQss); }
QString tool_cluster_style() { return QString::fromLatin1(kToolClusterQss); }
QString outline_pill_style() { return QString::fromLatin1(kOutlinePillQss); }
QString slider_style() { return QString::fromLatin1(kSliderQss); }

QIcon icon(const char* name) { return QIcon(new SvgIconEngine(QString::fromLatin1(name))); }

QIcon icon(const char* name, const QColor& normal) {
    return QIcon(new SvgIconEngine(QString::fromLatin1(name), normal));
}

QIcon raw_icon(const char* name) {
    return QIcon(new SvgIconEngine(QString::fromLatin1(name), QColor(), /*tint=*/false));
}

}  // namespace canvas::gui
