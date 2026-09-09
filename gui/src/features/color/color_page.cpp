#include "features/color/color_page.hpp"

#include "UX/MainWindow.hpp"
#include "ui_MainWindow.h"

#include "features/color/color_widgets.hpp"
#include "features/color/mini_timeline_strip.hpp"
#include "features/color/node_graph_canvas.hpp"

#include "UX/theme.hpp"

#include <QDockWidget>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QSplitter>
#include <QTabWidget>
#include <QToolBar>
#include <QToolButton>
#include <QVBoxLayout>
#include <QWidget>

#include <algorithm>
#include <utility>

namespace canvas::gui {

namespace {

// One placeholder stills/gallery cell: a rounded gradient well with an index
// and a timecode readout. Stands in for real captured thumbnails until the
// still-capture wiring lands (M1+).
QWidget* make_still_cell(int index, const QString& tc, QWidget* parent) {
    auto* cell = new QWidget(parent);
    cell->setFixedSize(98, 68);
    cell->setCursor(Qt::PointingHandCursor);
    apply_theme_style(cell, [index, tc] {
        const ThemeTokens& t = tokens();
        const QColor a = with_alpha(t.accent, 150 - 22 * (index % 3));
        const QColor lo = with_alpha(t.accent, 40);
        return QStringLiteral(
                   "QWidget { background: qlineargradient(x1:0, y1:0, x2:1, y2:1,"
                   " stop:0 %1, stop:1 %2); border: 1px solid %3; border-radius: 8px;"
                   " }")
            .arg(css(a), css(lo), css(t.border));
    });
    auto* layout = new QVBoxLayout(cell);
    layout->setContentsMargins(6, 4, 6, 4);
    layout->setSpacing(2);
    auto* idx = new QLabel(QString::number(index), cell);
    apply_theme_style(idx, [] {
        return QStringLiteral("color: %1; font-size: 13px; font-weight: 700;")
            .arg(css(tokens().on_accent));
    });
    auto* tc_label = new QLabel(tc, cell);
    apply_theme_style(tc_label, [] {
        return QStringLiteral("color: %1; font-size: 9px;")
            .arg(css(tokens().on_accent));
    });
    layout->addWidget(idx);
    layout->addStretch(1);
    layout->addWidget(tc_label);
    return cell;
}

// Panel header row shared by the side docks: bold title + optional hint on the
// right.
QWidget* make_dock_title(const QString& title, const QString& hint, QWidget* parent) {
    auto* row = new QWidget(parent);
    auto* layout = new QHBoxLayout(row);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(6);
    auto* lbl = new QLabel(title, row);
    apply_theme_style(lbl, [] {
        const ThemeTokens& t = tokens();
        return QStringLiteral("color: %1; font-size: 12px; font-weight: 600;")
            .arg(css(t.ink));
    });
    layout->addWidget(lbl, 1);
    if (!hint.isEmpty()) {
        auto* h = new QLabel(hint, row);
        apply_theme_style(h, [] {
            return QStringLiteral("color: %1; font-size: 10px;")
                .arg(css(tokens().ink_faint));
        });
        layout->addWidget(h);
    }
    return row;
}

QToolButton* make_tool(QWidget* parent, QLayout* target, const QIcon& ic,
                       const char* tip) {
    auto* b = new QToolButton(parent);
    b->setIcon(ic);
    b->setIconSize(QSize(15, 15));
    b->setToolTip(QObject::tr(tip));
    b->setCheckable(true);
    b->setAutoRaise(true);
    apply_theme_style(b, &flat_tool_style);
    target->addWidget(b);
    return b;
}

}  // namespace

void build_color_page(MainWindow& mw) {
    // ── Bottom workspace dock: mini strip > page toolbar > tool ribbon > grading splitter ──
    auto* workspace = new QWidget(&mw);
    workspace->setObjectName(QStringLiteral("colorWorkspace"));
    apply_theme_style(workspace, [] {
        return QStringLiteral("QWidget#colorWorkspace { background: transparent; }");
    });
    auto* root = new QVBoxLayout(workspace);
    root->setContentsMargins(6, 6, 6, 4);
    root->setSpacing(4);

    mw.color_mini_strip_ = new MiniTimelineStrip(workspace);
    root->addWidget(mw.color_mini_strip_);

    // ── Page toolbar (panel-visibility toggles, spec §Layout-2) ──
    auto* toolbar = new QWidget(workspace);
    toolbar->setObjectName(QStringLiteral("colorPageToolbar"));
    apply_theme_style(toolbar, [] {
        const ThemeTokens& t = tokens();
        return QStringLiteral(
            "QWidget#colorPageToolbar { background: %1; border: 1px solid %2;"
            " border-radius: 8px; }")
            .arg(css(t.surface_raised), css(t.border));
    });
    auto* toolbar_layout = new QHBoxLayout(toolbar);
    toolbar_layout->setContentsMargins(6, 3, 6, 3);
    toolbar_layout->setSpacing(4);

    auto* gallery_btn = make_tool(toolbar, toolbar_layout, icon("gallery"),
                                  "Gallery (still frames)");
    auto* luts_btn = make_tool(toolbar, toolbar_layout, icon("lut"),
                               "LUTs library");
    auto* pool_btn = make_tool(toolbar, toolbar_layout, icon("folder"),
                               "Media Pool");
    auto* clips_btn = make_tool(toolbar, toolbar_layout, icon("film-strip"),
                                "Clips (mini timeline)");
    clips_btn->setChecked(true);

    toolbar_layout->addStretch(1);

    auto* quick_export_btn = new QToolButton(toolbar);
    quick_export_btn->setText(MainWindow::tr("Quick Export"));
    quick_export_btn->setCheckable(true);
    quick_export_btn->setAutoRaise(true);
    apply_theme_style(quick_export_btn, &outline_pill_style);
    toolbar_layout->addWidget(quick_export_btn);

    auto* timeline_btn = make_tool(toolbar, toolbar_layout, icon("viewport"),
                                   "Timeline (show/hide the full timeline)");
    auto* nodes_btn = make_tool(toolbar, toolbar_layout, icon("nodes"),
                                "Nodes (node graph)");
    auto* effects_btn = make_tool(toolbar, toolbar_layout, icon("effects"),
                                  "Effects");
    auto* lightbox_btn = make_tool(toolbar, toolbar_layout, icon("lightbox"),
                                   "Lightbox");
    root->addWidget(toolbar);

    // ── Tool ribbon (viewer-overlay toggles, spec §Layout-6) ──
    auto* ribbon = new QWidget(workspace);
    ribbon->setObjectName(QStringLiteral("colorToolRibbon"));
    apply_theme_style(ribbon, [] {
        const ThemeTokens& t = tokens();
        return QStringLiteral(
            "QWidget#colorToolRibbon { background: %1; border: 1px solid %2;"
            " border-radius: 8px; }")
            .arg(css(t.surface_low), css(t.border_soft));
    });
    auto* ribbon_layout = new QHBoxLayout(ribbon);
    ribbon_layout->setContentsMargins(6, 2, 6, 2);
    ribbon_layout->setSpacing(4);

    auto* grid_btn = make_tool(ribbon, ribbon_layout, icon("grid"),
                               "Grid overlay");
    grid_btn->setChecked(true);
    auto* hdr_btn = make_tool(ribbon, ribbon_layout, icon("hdr"),
                              "HDR (highlight clipping)");
    auto* waveform_btn = make_tool(ribbon, ribbon_layout, icon("waveform"),
                                   "Waveform overlay on monitor");
    auto* vectorscope_btn = make_tool(ribbon, ribbon_layout, icon("vectorscope"),
                                      "Vectorscope overlay on monitor");
    auto* eyedropper_btn = make_tool(ribbon, ribbon_layout, icon("eyedropper"),
                                     "Eyedropper (sample color)");

    ribbon_layout->addStretch(1);

    auto* rcm_btn = new QToolButton(ribbon);
    rcm_btn->setText(MainWindow::tr("Enable RCM"));
    rcm_btn->setCheckable(true);
    rcm_btn->setAutoRaise(true);
    apply_theme_style(rcm_btn, &outline_pill_style);
    ribbon_layout->addWidget(rcm_btn);

    auto* proxy_btn = make_tool(ribbon, ribbon_layout, icon("proxy"),
                                "Proxy media");
    auto* stereo_btn = new QToolButton(ribbon);
    stereo_btn->setText(MainWindow::tr("3D"));
    stereo_btn->setCheckable(true);
    stereo_btn->setAutoRaise(true);
    apply_theme_style(stereo_btn, &outline_pill_style);
    ribbon_layout->addWidget(stereo_btn);
    root->addWidget(ribbon);

    // ── Grading workspace: Wheels | Curves | Scopes ──
    auto* splitter = new QSplitter(Qt::Horizontal, workspace);
    splitter->setChildrenCollapsible(false);
    auto* wheels = new ColorWheelsPanel(splitter);
    auto* curves = new CurvesPanel(splitter);
    auto* scopes = new ScopesPanel(splitter);
    wheels->setMinimumWidth(300);
    curves->setMinimumWidth(220);
    scopes->setMinimumWidth(240);
    splitter->addWidget(wheels);
    splitter->addWidget(curves);
    splitter->addWidget(scopes);
    splitter->setStretchFactor(0, 1);
    splitter->setStretchFactor(1, 1);
    splitter->setStretchFactor(2, 1);
    splitter->setSizes({460, 280, 340});
    root->addWidget(splitter, 1);

    // Scopes panel is fed by the same "frame re-rendered" signal the preview
    // viewer listens to, so it shows the presented (graded/mixed) frame, not
    // the raw source (rgb-parade spec §5). Receiver context is the panel itself
    // so the connection drops when the color page is torn down.
    QObject::connect(&mw.controller_, &SequenceController::frame_ready, scopes,
            [scopes](canvas::core::RenderFramePtr frame) {
                scopes->update_frame(std::move(frame));
            });

    mw.color_dock_ = new QDockWidget(MainWindow::tr("Color Workspace"), &mw);
    mw.color_dock_->setObjectName(QStringLiteral("colorDock"));
    apply_theme_style(mw.color_dock_, &dock_glow_style);
    mw.color_dock_->setWidget(workspace);
    mw.color_dock_->setFeatures(QDockWidget::DockWidgetMovable |
                                QDockWidget::DockWidgetFloatable);
    mw.color_dock_->setMinimumHeight(280);
    mw.addDockWidget(Qt::BottomDockWidgetArea, mw.color_dock_);
    mw.color_dock_->hide();

    // ── Left dock: Gallery | LUTs tabs ──
    auto* color_tabs = new QTabWidget(&mw);
    color_tabs->setObjectName(QStringLiteral("colorLeftTabs"));
    color_tabs->setTabPosition(QTabWidget::North);
    color_tabs->setMinimumWidth(224);
    color_tabs->setDocumentMode(true);
    apply_theme_style(color_tabs, &left_tab_strip_style);

    auto* gallery_tab = new QWidget(color_tabs);
    auto* gallery_root = new QVBoxLayout(gallery_tab);
    gallery_root->setContentsMargins(8, 8, 8, 8);
    gallery_root->setSpacing(8);
    auto* gallery_header = new QHBoxLayout;
    auto* gallery_title = new QLabel(MainWindow::tr("Gallery"), gallery_tab);
    apply_theme_style(gallery_title, [] {
        const ThemeTokens& t = tokens();
        return QStringLiteral("color: %1; font-size: 12px; font-weight: 600;")
            .arg(css(t.ink));
    });
    auto* gallery_add = new QToolButton(gallery_tab);
    gallery_add->setText(MainWindow::tr("Add"));
    gallery_add->setAutoRaise(true);
    apply_theme_style(gallery_add, &outline_pill_style);
    auto* gallery_clear = new QToolButton(gallery_tab);
    gallery_clear->setIcon(icon("reset"));
    gallery_clear->setIconSize(QSize(13, 13));
    gallery_clear->setAutoRaise(true);
    apply_theme_style(gallery_clear, &flat_tool_style);
    gallery_header->addWidget(gallery_title, 1);
    gallery_header->addWidget(gallery_add);
    gallery_header->addWidget(gallery_clear);
    gallery_root->addLayout(gallery_header);

    auto* stills = new QWidget(gallery_tab);
    auto* stills_grid = new QGridLayout(stills);
    stills_grid->setContentsMargins(0, 0, 0, 0);
    stills_grid->setSpacing(6);
    const char* const kStillTc[6] = {"00:00:01:05", "00:00:03:18", "00:00:05:02",
                                     "00:00:08:14", "00:00:11:27", "00:00:15:09"};
    for (int i = 0; i < 6; ++i) {
        stills_grid->addWidget(make_still_cell(i + 1, kStillTc[i], stills),
                               i / 2, i % 2, Qt::AlignLeft);
    }
    gallery_root->addWidget(stills, 1, Qt::AlignTop);

    auto* luts_tab = new QWidget(color_tabs);
    auto* luts_root = new QVBoxLayout(luts_tab);
    luts_root->setContentsMargins(8, 8, 8, 8);
    luts_root->setSpacing(8);
    auto* luts_search = new QLineEdit(luts_tab);
    luts_search->setPlaceholderText(MainWindow::tr("Search LUTs…"));
    luts_search->setClearButtonEnabled(true);
    luts_search->addAction(icon("search"), QLineEdit::LeadingPosition);
    apply_theme_style(luts_search, [] {
        const ThemeTokens& t = tokens();
        return QStringLiteral(
            "QLineEdit { background-color: %1; border: 1px solid %2;"
            " border-radius: 6px; padding: 4px 8px; color: %3; font-size: 12px;}"
            "QLineEdit:focus { border-color: %4; }")
            .arg(css(t.surface_low), css(t.border), css(t.ink), css(t.accent));
    });
    luts_root->addWidget(luts_search);
    auto* luts_list = new QListWidget(luts_tab);
    luts_list->setObjectName(QStringLiteral("lutsList"));
    luts_list->addItems({MainWindow::tr("Rec.709 → Rec.709 (identity)"),
                         MainWindow::tr("Rec.709 → DCI P3"),
                         MainWindow::tr("ACES Cineon"),
                         MainWindow::tr("Kodak 2383"),
                         MainWindow::tr("Fuji 3510"),
                         MainWindow::tr("Kodak 2393"),
                         MainWindow::tr("BT.1886 monitor curve")});
    apply_theme_style(luts_list, [] {
        const ThemeTokens& t = tokens();
        return QStringLiteral(
            "QListWidget#lutsList { background-color: %1; border: 1px solid %2;"
            " border-radius: 8px; color: %3; font-size: 12px; padding: 4px;}"
            "QListWidget#lutsList::item { padding: 6px 8px; border-radius: 6px;}"
            "QListWidget#lutsList::item:selected { background-color: %4; color: %5;}")
            .arg(css(t.surface_low), css(t.border), css(t.ink),
                 css(t.accent_soft), css(t.ink));
    });
    luts_root->addWidget(luts_list, 1);
    auto* luts_add = new QToolButton(luts_tab);
    luts_add->setText(MainWindow::tr("Add LUT"));
    luts_add->setAutoRaise(true);
    apply_theme_style(luts_add, &outline_pill_style);
    luts_root->addWidget(luts_add, 0, Qt::AlignLeft);

    color_tabs->addTab(gallery_tab, MainWindow::tr("Gallery"));
    color_tabs->addTab(luts_tab, MainWindow::tr("LUTs"));

    mw.color_left_dock_ = new QDockWidget(MainWindow::tr("Color Panels"), &mw);
    mw.color_left_dock_->setObjectName(QStringLiteral("colorLeftDock"));
    mw.color_left_dock_->setWidget(color_tabs);
    mw.color_left_dock_->setFeatures(QDockWidget::DockWidgetMovable |
                                     QDockWidget::DockWidgetFloatable);
    mw.color_left_dock_->setMinimumWidth(200);
    mw.addDockWidget(Qt::LeftDockWidgetArea, mw.color_left_dock_);
    mw.color_left_dock_->hide();

    // ── Right dock: Node Graph ──
    auto* node_root = new QWidget(&mw);
    auto* node_layout = new QVBoxLayout(node_root);
    node_layout->setContentsMargins(8, 8, 8, 8);
    node_layout->setSpacing(6);
    node_layout->addWidget(make_dock_title(MainWindow::tr("Node Graph"),
                                           MainWindow::tr("right-click to add"),
                                           node_root));
    auto* node_canvas = new NodeGraphCanvas(node_root);
    node_layout->addWidget(node_canvas, 1);
    mw.color_nodes_dock_ = new QDockWidget(MainWindow::tr("Nodes"), &mw);
    mw.color_nodes_dock_->setObjectName(QStringLiteral("colorNodesDock"));
    mw.color_nodes_dock_->setWidget(node_root);
    mw.color_nodes_dock_->setFeatures(QDockWidget::DockWidgetMovable |
                                      QDockWidget::DockWidgetFloatable);
    mw.color_nodes_dock_->setMinimumWidth(320);
    mw.addDockWidget(Qt::RightDockWidgetArea, mw.color_nodes_dock_);
    mw.color_nodes_dock_->hide();

    // ── Right dock: Effects ──
    auto* effects_root = new QWidget(&mw);
    auto* effects_layout = new QVBoxLayout(effects_root);
    effects_layout->setContentsMargins(8, 8, 8, 8);
    effects_layout->setSpacing(6);
    effects_layout->addWidget(make_dock_title(MainWindow::tr("Effects"),
                                              MainWindow::tr("built-in + OFX"),
                                              effects_root));
    auto* effects_search = new QLineEdit(effects_root);
    effects_search->setPlaceholderText(MainWindow::tr("Search effects…"));
    effects_search->setClearButtonEnabled(true);
    effects_search->addAction(icon("search"), QLineEdit::LeadingPosition);
    apply_theme_style(effects_search, [] {
        const ThemeTokens& t = tokens();
        return QStringLiteral(
            "QLineEdit { background-color: %1; border: 1px solid %2;"
            " border-radius: 6px; padding: 4px 8px; color: %3; font-size: 12px;}"
            "QLineEdit:focus { border-color: %4; }")
            .arg(css(t.surface_low), css(t.border), css(t.ink), css(t.accent));
    });
    effects_layout->addWidget(effects_search);
    auto* effects_list = new QListWidget(effects_root);
    effects_list->setObjectName(QStringLiteral("lutsList"));
    effects_list->addItems({MainWindow::tr("Blur"),
                            MainWindow::tr("Gaussian / Light Rays"),
                            MainWindow::tr("Glow"), MainWindow::tr("Sharpen"),
                            MainWindow::tr("Film Grain"),
                            MainWindow::tr("Color Transform"),
                            MainWindow::tr("Tilt / Defocus")});
    apply_theme_style(effects_list, [] {
        const ThemeTokens& t = tokens();
        return QStringLiteral(
            "QListWidget#lutsList { background-color: %1; border: 1px solid %2;"
            " border-radius: 8px; color: %3; font-size: 12px; padding: 4px;}"
            "QListWidget#lutsList::item { padding: 6px 8px; border-radius: 6px;}"
            "QListWidget#lutsList::item:selected { background-color: %4; color: %5;}")
            .arg(css(t.surface_low), css(t.border), css(t.ink),
                 css(t.accent_soft), css(t.ink));
    });
    effects_layout->addWidget(effects_list, 1);
    mw.color_effects_dock_ = new QDockWidget(MainWindow::tr("Effects"), &mw);
    mw.color_effects_dock_->setObjectName(QStringLiteral("colorEffectsDock"));
    mw.color_effects_dock_->setWidget(effects_root);
    mw.color_effects_dock_->setFeatures(QDockWidget::DockWidgetMovable |
                                        QDockWidget::DockWidgetFloatable);
    mw.color_effects_dock_->setMinimumWidth(240);
    mw.addDockWidget(Qt::RightDockWidgetArea, mw.color_effects_dock_);
    mw.color_effects_dock_->hide();

    // ── Right dock: Lightbox ──
    auto* lightbox_root = new QWidget(&mw);
    auto* lightbox_layout = new QVBoxLayout(lightbox_root);
    lightbox_layout->setContentsMargins(8, 8, 8, 8);
    lightbox_layout->setSpacing(6);
    lightbox_layout->addWidget(make_dock_title(MainWindow::tr("Lightbox"),
                                               MainWindow::tr("stills walls"),
                                               lightbox_root));
    auto* lightbox_grid_widget = new QWidget(lightbox_root);
    auto* lightbox_grid = new QGridLayout(lightbox_grid_widget);
    lightbox_grid->setContentsMargins(0, 0, 0, 0);
    lightbox_grid->setSpacing(6);
    for (int i = 0; i < 8; ++i) {
        lightbox_grid->addWidget(make_still_cell(i + 1,
                                                 QStringLiteral("00:0%1:%2:0%3")
                                                     .arg((i / 3) + 1)
                                                     .arg(i * 2 % 60)
                                                     .arg(i % 3),
                                                 lightbox_grid_widget),
                                 i / 3, i % 3, Qt::AlignLeft);
    }
    lightbox_layout->addWidget(lightbox_grid_widget, 1, Qt::AlignTop);
    mw.color_lightbox_dock_ = new QDockWidget(MainWindow::tr("Lightbox"), &mw);
    mw.color_lightbox_dock_->setObjectName(QStringLiteral("colorLightboxDock"));
    mw.color_lightbox_dock_->setWidget(lightbox_root);
    mw.color_lightbox_dock_->setFeatures(QDockWidget::DockWidgetMovable |
                                         QDockWidget::DockWidgetFloatable);
    mw.color_lightbox_dock_->setMinimumWidth(280);
    mw.addDockWidget(Qt::RightDockWidgetArea, mw.color_lightbox_dock_);
    mw.color_lightbox_dock_->hide();

    // ── Wiring ──
    if (mw.project_) mw.color_mini_strip_->set_sequence(&mw.project_->sequence);
    QObject::connect(&mw.controller_, &SequenceController::position_changed, &mw,
            [&mw](int64_t frame) {
                if (mw.color_mini_strip_) mw.color_mini_strip_->set_playhead(frame);
            });
    QObject::connect(mw.color_mini_strip_, &MiniTimelineStrip::clip_activated, &mw,
            [&mw](canvas::core::ClipId, int64_t frame) { mw.controller_.seek(frame); });

    // Page-toolbar toggles → panel visibility.
    // The media panel opens compact (~20% of the page) once, so the grading
    // tools at the bottom keep the width — afterwards it drags freely like the
    // edit-tab media pool and the size is preserved.
    bool left_panels_sized = false;
    const auto show_left_panel = [&mw, color_tabs, gallery_btn, luts_btn,
                                  &left_panels_sized](bool, int tab) {
        color_tabs->setCurrentIndex(tab);
        mw.color_left_dock_->setVisible(gallery_btn->isChecked() || luts_btn->isChecked());
        if (!left_panels_sized && mw.color_left_dock_->isVisible()) {
            left_panels_sized = true;
            const int target = std::clamp(mw.width() / 5, 220, 420);
            mw.resizeDocks({mw.color_left_dock_}, {target}, Qt::Horizontal);
        }
    };
    QObject::connect(gallery_btn, &QToolButton::toggled, &mw,
            [luts_btn, show_left_panel](bool on) {
                if (on && luts_btn->isChecked()) luts_btn->setChecked(false);
                show_left_panel(on, 0);
            });
    QObject::connect(luts_btn, &QToolButton::toggled, &mw,
            [gallery_btn, show_left_panel](bool on) {
                if (on && gallery_btn->isChecked()) gallery_btn->setChecked(false);
                show_left_panel(on, 1);
            });
    QObject::connect(pool_btn, &QToolButton::toggled, &mw,
            [&mw](bool on) { if (mw.media_dock_) mw.media_dock_->setVisible(on); });
    QObject::connect(clips_btn, &QToolButton::toggled, &mw,
            [&mw](bool on) { if (mw.color_mini_strip_) mw.color_mini_strip_->setVisible(on); });
    QObject::connect(quick_export_btn, &QToolButton::toggled, &mw,
            [&mw](bool on) { if (mw.deliver_settings_dock_) mw.deliver_settings_dock_->setVisible(on); });
    QObject::connect(timeline_btn, &QToolButton::toggled, &mw,
            [&mw](bool on) { mw.ui->timelineDock->setVisible(on); });
    QObject::connect(nodes_btn, &QToolButton::toggled, &mw,
            [&mw](bool on) { if (mw.color_nodes_dock_) mw.color_nodes_dock_->setVisible(on); });
    QObject::connect(effects_btn, &QToolButton::toggled, &mw,
            [&mw](bool on) { if (mw.color_effects_dock_) mw.color_effects_dock_->setVisible(on); });
    QObject::connect(lightbox_btn, &QToolButton::toggled, &mw,
            [&mw](bool on) { if (mw.color_lightbox_dock_) mw.color_lightbox_dock_->setVisible(on); });

    // Tool-ribbon toggles are visual state only for now (M0 scaffold); the
    // overlay plumbing lands with the grade backend.
    Q_UNUSED(hdr_btn);
    Q_UNUSED(waveform_btn);
    Q_UNUSED(vectorscope_btn);
    Q_UNUSED(eyedropper_btn);
    Q_UNUSED(grid_btn);
    Q_UNUSED(proxy_btn);
}

void enter_color_page(MainWindow& mw) {
    mw.color_active_ = true;
    if (mw.media_dock_) mw.media_dock_->hide();
    if (mw.inspector_dock_) mw.inspector_dock_->hide();
    if (mw.deliver_settings_dock_) mw.deliver_settings_dock_->hide();
    if (mw.deliver_queue_dock_) mw.deliver_queue_dock_->hide();
    mw.ui->timelineDock->hide();
    if (mw.color_dock_) mw.color_dock_->show();
    if (mw.color_mini_strip_) {
        if (mw.project_) mw.color_mini_strip_->set_sequence(&mw.project_->sequence);
        mw.color_mini_strip_->set_playhead(mw.controller_.current_frame());
    }
    if (mw.status_) {
        mw.status_->showMessage(MainWindow::tr("Color: build and refine the look."));
    }
}

void leave_color_page(MainWindow& mw) {
    if (!mw.color_active_) return;
    mw.color_active_ = false;
    if (mw.color_dock_) mw.color_dock_->hide();
    if (mw.color_left_dock_) mw.color_left_dock_->hide();
    if (mw.color_nodes_dock_) mw.color_nodes_dock_->hide();
    if (mw.color_effects_dock_) mw.color_effects_dock_->hide();
    if (mw.color_lightbox_dock_) mw.color_lightbox_dock_->hide();
    mw.ui->timelineDock->show();
    // Contextual edit tools ride with the page: they come back exactly as the
    // Edit/Deliver pages expect them.
    if (QToolBar* tools = mw.findChild<QToolBar*>(QStringLiteral("contextualTools")))
        tools->show();
}

}  // namespace canvas::gui