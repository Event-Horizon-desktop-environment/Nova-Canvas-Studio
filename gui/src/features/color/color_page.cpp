#include "features/color/color_page.hpp"

#include "UX/MainWindow.hpp"
#include "ui_MainWindow.h"

#include "features/color/color_widgets.hpp"
#include "features/color/curves/curves_panel.hpp"
#include "features/color/mini_timeline_strip.hpp"
#include "features/color/node_graph_canvas.hpp"

#include "UX/theme.hpp"

#include "canvas/core/colorsci/histogram.hpp"
#include "canvas/core/export/grade_frame.hpp"
#include "canvas/core/grade_graph/graph.hpp"
#include "canvas/core/timeline/edit_ops.hpp"
#include "features/playback/sync_constants.hpp"

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
#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

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

namespace {

using canvas::core::colorsci::CurveParams;
using canvas::core::colorsci::WheelPanelState;
using canvas::core::grade_graph::GradeGraph;

// Serializes the panels' combined state into a two-node grade chain: the
// wheels' Primaries LGG corrector, then a Curves corrector when the curve law
// is non-identity (identity is dropped so the JSON stays byte-compatible with
// Phase 4's single-corrector files), wired to the output. The
// panel→graph→edit-op handshake is unchanged from Phase 4.
GradeGraph make_grade_graph(const WheelPanelState& state, const CurveParams& curves) {
    using canvas::core::grade_graph::CorrectMode;
    using canvas::core::grade_graph::NodeKind;

    GradeGraph g;
    const int lgg_node = g.add_node(NodeKind::kCorrector);
    g.node(lgg_node).correct_mode = CorrectMode::kLgg;
    g.node(lgg_node).lgg = state.lgg;

    int tail = lgg_node;
    if (!curves.is_identity()) {
        // Re-fetch Node& by id after every add_node: the vector reallocates.
        const int cv = g.add_node(NodeKind::kCorrector);
        g.node(cv).correct_mode = CorrectMode::kCurves;
        g.node(cv).curves = curves;
        if (g.add_rgb_edge(tail, cv) < 0) {
            g.clear();
            return g;
        }
        tail = cv;
    }

    const int out = g.add_node(NodeKind::kOutput);
    if (g.add_rgb_edge(tail, out) < 0) {
        g.clear();
        return g;
    }
    return g;
}

// Reverses make_grade_graph: pulls the LGG + Curves params a clip's tree owns
// back into the panels (identity defaults when a mode is absent).
struct GradeLoadState {
    WheelPanelState wheels;
    CurveParams curves;
};

GradeLoadState grade_load_state(const GradeGraph& graph) {
    using canvas::core::grade_graph::CorrectMode;
    GradeLoadState out;
    for (std::size_t i = 0; i < graph.num_nodes(); ++i) {
        const auto& n = graph.node(static_cast<int>(i));
        switch (n.correct_mode) {
            case CorrectMode::kLgg:
                if (n.lgg) out.wheels.lgg = *n.lgg;
                break;
            case CorrectMode::kCurves:
                if (n.curves) out.curves = *n.curves;
                break;
            default:
                break;
        }
    }
    return out;
}

const canvas::core::Clip* find_clip_by_id(const canvas::core::Sequence& seq,
                                          canvas::core::ClipId id) {
    for (const auto& track : seq.video_tracks) {
        for (const auto& clip : track.clips) {
            if (clip.id == id) return &clip;
        }
    }
    for (const auto& track : seq.audio_tracks) {
        for (const auto& clip : track.clips) {
            if (clip.id == id) return &clip;
        }
    }
    return nullptr;
}

// Integer-stride box-filter downscale into a NEW frame (the source may be a
// shared cache frame, so never alias). Used to keep per-frame grade + scope
// evaluation on a preview-resolution budget.
canvas::core::VideoFramePtr downscale_rgba(const canvas::core::VideoFrame& src,
                                           int max_dim) {
    const int longest = std::max(src.width, src.height);
    if (longest <= max_dim) {
        return std::make_shared<canvas::core::VideoFrame>(src);
    }
    const int stride = (longest + max_dim - 1) / max_dim;
    const int dw = std::max(1, src.width / stride);
    const int dh = std::max(1, src.height / stride);

    auto out = std::make_shared<canvas::core::VideoFrame>();
    out->width = dw;
    out->height = dh;
    out->stride = static_cast<std::size_t>(dw) * 4u;
    out->rgba.resize(static_cast<std::size_t>(dw * dh) * 4u);
    for (int y = 0; y < dh; ++y) {
        for (int x = 0; x < dw; ++x) {
            std::uint32_t r = 0, g = 0, b = 0;
            int count = 0;
            const int y_hi = std::min((y + 1) * stride, src.height);
            const int x_hi = std::min((x + 1) * stride, src.width);
            for (int sy = y * stride; sy < y_hi; ++sy) {
                const std::uint8_t* row = &src.rgba[static_cast<std::size_t>(sy) * src.stride];
                for (int sx = x * stride; sx < x_hi; ++sx) {
                    const std::uint8_t* px = row + static_cast<std::size_t>(sx) * 4u;
                    r += px[0];
                    g += px[1];
                    b += px[2];
                    ++count;
                }
            }
            std::uint8_t* op = &out->rgba[static_cast<std::size_t>(y) * out->stride +
                                          static_cast<std::size_t>(x) * 4u];
            op[0] = static_cast<std::uint8_t>(r / count);
            op[1] = static_cast<std::uint8_t>(g / count);
            op[2] = static_cast<std::uint8_t>(b / count);
            op[3] = 255;
        }
    }
    return out;
}

// Normalized per-column luma occupancy (0..1) for the curve editor's veil.
std::vector<float> luma_veil(const canvas::core::VideoFrame& frame) {
    using canvas::core::colorsci::ColumnHistogram;
    using canvas::core::colorsci::kHistogramCols;
    using canvas::core::colorsci::kHistogramLevels;
    ColumnHistogram h;
    h.accumulate(frame);
    std::vector<float> out(static_cast<std::size_t>(kHistogramCols), 0.0f);
    float peak = 0.0f;
    for (int c = 0; c < kHistogramCols; ++c) {
        std::uint64_t sum = 0;
        const std::size_t base = static_cast<std::size_t>(c) * kHistogramLevels;
        for (int l = 0; l < kHistogramLevels; ++l) {
            sum += h.luma()[base + static_cast<std::size_t>(l)];
        }
        out[static_cast<std::size_t>(c)] = static_cast<float>(sum);
        peak = std::max(peak, out[static_cast<std::size_t>(c)]);
    }
    for (float& v : out) v = peak > 0.0f ? v / peak : 0.0f;
    return out;
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

    // ── Vertical divider: mini strip (media/clips) on top, page body below ──
    auto* page_splitter = new QSplitter(Qt::Vertical, workspace);
    page_splitter->setObjectName(QStringLiteral("colorPageSplitter"));
    page_splitter->setChildrenCollapsible(true);
    page_splitter->setHandleWidth(6);

    mw.color_mini_strip_ = new MiniTimelineStrip(page_splitter);
    mw.color_mini_strip_->set_thumbnail_service(&mw.thumbnails_);
    page_splitter->addWidget(mw.color_mini_strip_);

    auto* page_body = new QWidget(page_splitter);
    auto* body_root = new QVBoxLayout(page_body);
    body_root->setContentsMargins(0, 0, 0, 0);
    body_root->setSpacing(4);

    // ── Page toolbar (panel-visibility toggles, spec §Layout-2) ──
    auto* toolbar = new QWidget(page_body);
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
    body_root->addWidget(toolbar);

    // ── Tool ribbon (viewer-overlay toggles, spec §Layout-6) ──
    auto* ribbon = new QWidget(page_body);
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
    body_root->addWidget(ribbon);

// ── Grading workspace: Wheels | Curves | Scopes ──
    // Wheels/curves/scopes live INSIDE the strip splitter's bottom cell (below
    // the toolbar + ribbon), so they are the flexible space: dragging the strip
    // divider grows/shrinks the whole section below it — toolbar and ribbon stay
    // fixed and fully visible while the grading area absorbs the change — and
    // dragging the dock's top edge resizes the entire section together.
    auto* wheels = new ColorWheelsPanel(&mw);
    auto* curves = new CurvesPanel(&mw);
    auto* scopes = new ScopesPanel(&mw);

    // ── Close the vertical splitter: strip above, everything else below ──
    page_splitter->addWidget(page_body);
    page_splitter->setStretchFactor(0, 0);
    page_splitter->setStretchFactor(1, 1);
    page_splitter->setSizes({102, 2000});

    auto* grading_splitter = new QSplitter(Qt::Horizontal, page_body);
    grading_splitter->setObjectName(QStringLiteral("colorGradingSplitter"));
    grading_splitter->setHandleWidth(6);
    grading_splitter->addWidget(wheels);
    grading_splitter->addWidget(curves);
    grading_splitter->addWidget(scopes);
    grading_splitter->setSizes({460, 280, 340});
    body_root->addWidget(grading_splitter, 1);

    root->addWidget(page_splitter, 1);

    // Scopes panel is fed by the same "frame re-rendered" signal the preview
    // viewer listens to, but with the SELECTED clip's grade applied first
    // (rgb-parade spec §5: the scopes show the graded signal, not the raw
    // source). The grade runs at the preview-resolution cap on a box-filtered
    // copy so the per-frame evaluation stays cheap and never aliases the
    // presenter's cached frame; the same downscaled result refreshes the curve
    // editor's luma veil. With no selection/no grade/no CPU frame the scopes
    // get the raw presented frame, as before. Receiver context is the panel
    // itself so the connection drops when the color page is torn down.
    QObject::connect(&mw.controller_, &SequenceController::frame_ready, scopes,
            [&mw, scopes, curves](canvas::core::RenderFramePtr frame) {
                if (mw.project_ && frame && frame->a) {
                    canvas::core::grade_graph::GradeGraph grade;
                    canvas::core::Track::Kind kind;
                    std::size_t index = 0;
                    canvas::core::Clip clip;
                    if (mw.find_selected_clip(kind, index, clip) && clip.has_grade()) {
                        grade = clip.grade;
                    }
                    if (!grade.edges().empty()) {
                        const auto scaled =
                            downscale_rgba(*frame->a, canvas::gui::kPreviewMaxDim);
                        const auto graded =
                            canvas::core::apply_grade_to_frame(*scaled, grade);
                        if (graded) {
                            curves->set_veil(luma_veil(*graded));
                            auto rf = std::make_shared<canvas::core::RenderFrame>();
                            rf->a = graded;
                            scopes->update_frame(std::move(rf));
                            return;
                        }
                    }
                }
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
            [&mw, wheels, curves, node_canvas](canvas::core::ClipId id, int64_t frame) {
                mw.controller_.seek(frame);
                mw.activate_color_clip(id);
                // Load the activated clip's grade into the panels and the node
                // canvas so the page edits what it shows.
                if (!mw.project_) return;
                const canvas::core::Clip* clip = find_clip_by_id(mw.project_->sequence, id);
                if (!clip) return;
                const GradeLoadState state = grade_load_state(clip->grade);
                wheels->set_state(state.wheels);
                curves->set_params(state.curves);
                node_canvas->load_graph(clip->grade);
            });
    // Drag-to-scrub: begin_scrub on grab, fast low-res preview on move, clean
    // full-res commit on release.
    QObject::connect(mw.color_mini_strip_, &MiniTimelineStrip::scrub_begin, &mw,
            [&mw]() { mw.controller_.begin_scrub(); });
    QObject::connect(mw.color_mini_strip_, &MiniTimelineStrip::scrubbed, &mw,
            [&mw](int64_t frame) { mw.controller_.seek_preview(frame); });
    QObject::connect(mw.color_mini_strip_, &MiniTimelineStrip::scrub_committed, &mw,
            [&mw](int64_t frame) {
                mw.controller_.end_scrub();
                mw.controller_.seek(frame);
            });

    // Wheel/curve commits → ONE undoable set_clip_grade on the selected clip.
    // Relay through mw so the panels stay thin views: the Color page owns the
    // graph law, the edit-op, and the undo recording. Both panels commit the
    // COMBINED state (wheels → lgg, curves → curve law) so edits in one panel
    // never discard the other.
    const auto commit_grade = [&mw, wheels, curves] {
        canvas::core::Track::Kind kind;
        std::size_t index = 0;
        canvas::core::Clip clip;
        if (!mw.find_selected_clip(kind, index, clip)) return;
        const canvas::core::grade_graph::GradeGraph g =
            make_grade_graph(wheels->state(), curves->params());
        auto cmd = canvas::core::set_clip_grade(mw.project_->sequence, kind,
                index, clip.id, g);
        if (!cmd) return;
        mw.undo_.record(std::move(cmd));
        mw.has_unsaved_changes_ = true;
        mw.refresh_timeline();
        mw.push_snapshot();
    };
    // Live movement previews through the same law without an undo entry.
    const auto preview_grade = [&mw, wheels, curves] {
        canvas::core::Track::Kind kind;
        std::size_t index = 0;
        canvas::core::Clip clip;
        if (!mw.find_selected_clip(kind, index, clip)) return;
        const canvas::core::grade_graph::GradeGraph g =
            make_grade_graph(wheels->state(), curves->params());
        auto cmd = canvas::core::set_clip_grade(mw.project_->sequence, kind,
                index, clip.id, g);
        if (!cmd) return;
        cmd->redo(mw.project_->sequence);
        mw.refresh_timeline();
        mw.push_snapshot();
    };
    QObject::connect(wheels, &ColorWheelsPanel::params_committed, &mw, commit_grade);
    QObject::connect(wheels, &ColorWheelsPanel::params_preview, &mw, preview_grade);
    QObject::connect(curves, &CurvesPanel::curves_committed, &mw, commit_grade);
    QObject::connect(curves, &CurvesPanel::curves_preview, &mw, preview_grade);

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