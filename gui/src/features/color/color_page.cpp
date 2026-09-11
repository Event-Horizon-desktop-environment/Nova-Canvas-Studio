#include "features/color/color_page.hpp"

#include "UX/MainWindow.hpp"
#include "ui_MainWindow.h"

#include "features/color/color_widgets.hpp"
#include "features/color/curves/curves_panel.hpp"
#include "features/color/mini_timeline_strip.hpp"
#include "features/color/node_graph_canvas.hpp"

#include "UX/theme.hpp"

#include "canvas/core/colorsci/histogram.hpp"
#include "canvas/core/grade_graph/graph.hpp"
#include "canvas/core/timeline/edit_ops.hpp"
#include "canvas/core/util/color_log.hpp"
#include "canvas/core/util/log.hpp"
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
#include <chrono>
#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

namespace canvas::gui {

namespace {

// Grade-preview snapshot throttle: the grade rides on the present path as a
// GPU-sampled 3D LUT, and preview ticks route through swap_project() — which
// keeps the decode stack warm and just re-presents the current frame (~2ms, a
// retain-hit), instead of rebuilding every decoder per SetProject (~217ms as the
// old push_snapshot() path did). 33ms ≈ one playhead cadence at 30fps, so a drag
// lands a fresh preview at realtime rate; SwapProject requests are also
// coalesced in the worker queue so only the newest survives while draining. The
// final params_committed still lands the exact end state as one undo step.
constexpr std::chrono::milliseconds kGradePreviewThrottle{33};

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

// Node census for the always-on [`grade]` trace: distinguishes "wheels only",
// "wheels + curves", and multi-node trees in one line.
QString graph_census(const GradeGraph& g) {
    using canvas::core::grade_graph::CorrectMode;
    int lgg = 0, curves = 0, other = 0;
    for (std::size_t i = 0; i < g.num_nodes(); ++i) {
        switch (g.node(static_cast<int>(i)).correct_mode) {
            case CorrectMode::kLgg: ++lgg; break;
            case CorrectMode::kCurves: ++curves; break;
            default: ++other; break;
        }
    }
    return QStringLiteral("nodes=%1 lgg=%2 curves=%3 other=%4")
        .arg(g.num_nodes()).arg(lgg).arg(curves).arg(other);
}

// Compact wheel/curve state digest for the commit + preview traces: the
// primaries LGG masters + per-channel lift, so a "return to center" commit
// (which now restores the last committed wheel) is readable as restore-offset
// versus the previous commit.
QString grade_state_digest(const WheelPanelState& state, const CurveParams& curves) {
    const auto& lgg = state.lgg;
    std::size_t curve_pts = 0;
    for (const auto& ch : curves.channels) curve_pts += ch.size();
    return QStringLiteral(
               "lift=(m=%1 r=%2 g=%3 b=%4) gamma=(m=%5) gain=(m=%6) off=(m=%7 r=%8 g=%9 b=%10)"
               " tones=(temp=%11 tint=%12 hue=%13 contrast=%14 pivot=%15 mid=%16 black=%17)"
               " curves=%18pts")
        .arg(QString::number(lgg.lift_master, 'f', 3), QString::number(lgg.lift_r, 'f', 3),
             QString::number(lgg.lift_g, 'f', 3), QString::number(lgg.lift_b, 'f', 3),
             QString::number(lgg.gamma_master, 'f', 3), QString::number(lgg.gain_master, 'f', 3),
             QString::number(state.offset.master, 'f', 3), QString::number(state.offset.r, 'f', 3),
             QString::number(state.offset.g, 'f', 3), QString::number(state.offset.b, 'f', 3),
             QString::number(state.temp, 'f', 1), QString::number(state.tint, 'f', 1),
             QString::number(state.hue_deg, 'f', 1), QString::number(state.contrast, 'f', 2),
             QString::number(state.pivot, 'f', 2), QString::number(state.mid_detail, 'f', 1),
             QString::number(state.black_offset, 'f', 3), QString::number(curve_pts));
}

// Serializes the panels' combined state into a two-node grade chain: the
// wheels' Primaries LGG corrector (with the Offset wheel's additive term applied
// before the LGG stage), then a Curves corrector when the curve law
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
    g.node(lgg_node).offset = state.offset;

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

    // Always-on graph-build trace: which correctors the panels' combined state
    // produced (curves dropped when identity). Distinguishes a wheels-only
    // build from a wheels+curves build at the graph boundary, matching the
    // [`grade]` population in commit_grade/preview_grade below.
    qWarning().nospace()
        << "[grade] graph-built " << graph_census(g)
        << " curves_in_panel=" << (curves.is_identity() ? 0 : 1);
    CANVAS_COLOR_LOG(
        "[graph] built nodes=%zu edges=%zu curves_in_panel=%d census=%s",
        g.num_nodes(), g.edges().size(), curves.is_identity() ? 0 : 1,
        graph_census(g).toStdString().c_str());
    return g;
}

// Reverses make_grade_graph: pulls the LGG + Offset + Curves params a clip's
// tree owns back into the panels (identity defaults when a mode is absent).
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
                if (n.offset) out.wheels.offset = *n.offset;
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
    // viewer listens to. The presenter path (TimelineDecoder::frame/preview)
    // now applies the clip-under-the-playhead's grade, so this handler just
    // relays the already-graded presented frame — the scopes show the graded
    // signal exactly as it leaves the preview path (rgb-parade spec §5), no
    // second grade here. The CPU frame also refreshes the curve editor's luma
    // veil. Receiver context is the panel itself so the connection drops when
    // the color page is torn down.
    QObject::connect(&mw.controller_, &SequenceController::frame_ready, scopes,
            [scopes, curves](canvas::core::RenderFramePtr frame) {
                if (frame && frame->a) {
                    curves->set_veil(luma_veil(*frame->a));
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
                // Selection trace so a click on the mini strip is visible in the
                // always-on log (distinguishes "clip selected" from "nothing
                // selected, fallback in play").
                qWarning().nospace()
                    << "[grade] activate clip=" << id << " frame=" << frame;
                // Load the activated clip's grade into the panels and the node
                // canvas so the page edits what it shows.
                if (!mw.project_) return;
                const canvas::core::Clip* clip = find_clip_by_id(mw.project_->sequence, id);
                if (!clip) return;
                const GradeLoadState state = grade_load_state(clip->grade);
                wheels->set_state(state.wheels);
                curves->set_params(state.curves);
                node_canvas->load_graph(clip->grade);
                // Load trace: boards and node canvas now reflect this clip's
                // saved grade. Digest shows the restore-to-center semantics as
                // "the wheels come up with whatever the clip last committed".
                qWarning().nospace()
                    << "[grade] header-load clip=" << id
                    << " " << graph_census(clip->grade)
                    << " " << grade_state_digest(state.wheels, state.curves);
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
    // Grade target: the selected clip, or — when nothing is selected — the
    // clip the viewer is actually showing (topmost video-track clip at the
    // playhead). Without the fallback, wheel/curve commits died silently at
    // find_selected_clip (selected_clip_ == 0), which read as "touching the
    // controls does nothing". On fallback the clip is selected too, so the
    // panels, undo and selection state stay in one consistent place.
    const auto resolve_grade_target =
        [&mw](canvas::core::Track::Kind& kind, std::size_t& index,
              canvas::core::Clip& clip) -> bool {
        if (mw.find_selected_clip(kind, index, clip)) {
            CANVAS_COLOR_LOG(
                "[target] resolve SELECTED kind=%d index=%zu clip=%llu frame=%lld",
                static_cast<int>(kind), static_cast<unsigned long long>(index),
                static_cast<unsigned long long>(clip.id),
                static_cast<long long>(mw.controller_.current_frame()));
            return true;
        }
        if (!mw.project_) {
            CANVAS_COLOR_LOG("[target] resolve NO-PROJECT");
            return false;
        }
        const auto& seq = mw.project_->sequence;
        const int64_t frame = mw.controller_.current_frame();
        for (std::size_t i = seq.video_tracks.size(); i-- > 0;) {
            const canvas::core::Clip* c = seq.video_tracks[i].clip_at(frame);
            if (!c || c->media < 0) continue;
            kind = canvas::core::Track::Kind::Video;
            index = i;
            clip = *c;
            mw.activate_color_clip(clip.id);
            qWarning().nospace()
                << "[grade] fallback no-selection -> playhead clip=" << clip.id
                << " track=" << index << " frame=" << frame;
            CANVAS_COLOR_LOG(
                "[target] resolve FALLBACK-PLAYHEAD track=%zu clip=%llu frame=%lld",
                static_cast<unsigned long long>(index),
                static_cast<unsigned long long>(clip.id),
                static_cast<long long>(frame));
            return true;
        }
        CANVAS_COLOR_LOG(
            "[target] resolve MISS frame=%lld tracks=%zu",
            static_cast<long long>(frame),
            static_cast<unsigned long long>(seq.video_tracks.size()));
        return false;
    };
    // Monotone change-token stamped onto every grade graph so the always-on
    // log chain (commit → bake → upload → draw) can be correlated by one seq.
    // Static: the lambdas below outlive this builder (they're connected with
    // &mw context), so a function-local would dangle and seq would read
    // recycled stack garbage in the log.
    static auto grade_seq = std::uint64_t{0};
    const auto commit_grade = [&mw, wheels, curves, resolve_grade_target] {
        canvas::core::Track::Kind kind;
        std::size_t index = 0;
        canvas::core::Clip clip;
        if (!resolve_grade_target(kind, index, clip)) {
            qWarning().nospace()
                << "[grade] no target: no selection and no video clip at playhead "
                << mw.controller_.current_frame();
            return;
        }
        canvas::core::grade_graph::GradeGraph g =
            make_grade_graph(wheels->state(), curves->params());
        g.change_seq = ++grade_seq;
        // Color archive: the clip's state BEFORE this commit lands (the last
        // committed grade is `clip.grade`; preview ticks have already mutated
        // the sequence, so this is the true pre-change snapshot as of release).
        CANVAS_COLOR_LOG(
            "[grade] pre-change seq=%llu kind=%d track=%zu clip=%llu frame=%lld "
            "prior=grade nodes=%zu edges=%zu has=%d state=%s",
            static_cast<unsigned long long>(g.change_seq), static_cast<int>(kind),
            static_cast<unsigned long long>(index),
            static_cast<unsigned long long>(clip.id),
            static_cast<long long>(mw.controller_.current_frame()),
            clip.grade.num_nodes(), clip.grade.edges().size(), clip.has_grade() ? 1 : 0,
            grade_state_digest(wheels->state(), curves->params()).toStdString().c_str());
        auto cmd = canvas::core::set_clip_grade(mw.project_->sequence, kind,
                index, clip.id, g);
        if (!cmd) return;
        mw.undo_.record(std::move(cmd));
        // Always-on commit trace: proves a wheel/curve/tone-field release
        // actually landed an edit, on which clip (the fallback selection
        // path from resolve_grade_target shows up here as the target clip),
        // and exactly what state was written (the digest is the only place
        // that shows a restore-to-center offset surviving as a real grade).
        qWarning().nospace()
            << "[grade] commit seq=" << g.change_seq
            << " t=" << canvas::core::log::epoch_ms()
            << " kind=" << static_cast<int>(kind)
            << " track=" << index << " clip=" << clip.id
            << " undo=" << mw.undo_.count()
            << " " << graph_census(g)
            << " " << grade_state_digest(wheels->state(), curves->params());
        CANVAS_COLOR_LOG(
            "[grade] commit seq=%llu kind=%d track=%zu clip=%llu frame=%lld "
            "new=grade nodes=%zu edges=%zu state=%s",
            static_cast<unsigned long long>(g.change_seq), static_cast<int>(kind),
            static_cast<unsigned long long>(index),
            static_cast<unsigned long long>(clip.id),
            static_cast<long long>(mw.controller_.current_frame()),
            g.num_nodes(), g.edges().size(),
            grade_state_digest(wheels->state(), curves->params()).toStdString().c_str());
        mw.has_unsaved_changes_ = true;
        mw.refresh_timeline();
        mw.push_grade_snapshot();
    };
    // Live movement previews through the same law without an undo entry.
    // THROTTLED: preview ticks re-present the current frame through warm decoders
    // (swap_project, ~2ms) instead of rebuilding the decode stack, so a realtime
    // ~30Hz cadence is affordable — but pushing on EVERY mouse-move is still
    // wasteful when the worker is mid-present, and only the NEWEST grade matters
    // while dragging. The throttle + queue coalescing keep the preview at display
    // rate without ever out-running the worker.
    // NOTE: never initialize these to time_point::min() — `now - min()` on a
    // nanosecond-rep steady_clock OVERFLOWS int64 and wraps negative, so
    // `now - last_preview_push < kGradePreviewThrottle` becomes permanently
    // true and the preview is throttled forever (the "color only applies on
    // drag release" bug). Seed them from wall "now" instead: the first drag
    // tick lands immediately, then a sane ~30Hz throttle cadence takes over.
    static auto last_preview_push = std::chrono::steady_clock::now() - kGradePreviewThrottle;
    static auto preview_drops = std::size_t{0};
    static auto last_preview_summary = std::chrono::steady_clock::now();
    // "Drag just started" detector for the color archive: any preview activity
    // (accepted OR throttled-dropped) refreshes this, so a quiet gap > 800ms
    // before an accepted tick means a NEW wheel/curve interaction began — the
    // point where the pre-change snapshot must be taken (by commit time the
    // sequence already carries the drag's results). Seeded 1s in the past so
    // the FIRST interaction reads as a fresh drag begin too.
    static auto last_preview_any =
        std::chrono::steady_clock::now() - std::chrono::seconds(1);
    const auto preview_grade = [&mw, wheels, curves, resolve_grade_target] {
        static constexpr auto kDragBeginGap = std::chrono::milliseconds(800);
        const auto now = std::chrono::steady_clock::now();
        const bool drag_begin = now - last_preview_any > kDragBeginGap;
        last_preview_any = now;
        // EXTREME-TRACE: every preview invocation, throttled or not, lands one
        // archive line. If wheel drags log `[wheels] move` lines but NO
        // `[preview] in` lines follow, the params_preview signal -> preview_grade
        // connection is dead; if `[preview] in ... throttled=1` appears forever
        // without a `[grade] preview seq=`, the throttle is stuck.
        const bool throttled = now - last_preview_push < kGradePreviewThrottle;
        CANVAS_COLOR_LOG(
            "[preview] in drag_begin=%d throttled=%d dropped=%zu throttle_ms=%lld",
            drag_begin ? 1 : 0, throttled ? 1 : 0, preview_drops,
            static_cast<long long>(kGradePreviewThrottle.count()));
        if (throttled) {
            // Throttle-drop counter: collapsed into one line when the drag
            // finally lands so the always-on log never floods at mouse-move
            // rate, but the ~30Hz effective preview cadence — and how stale
            // the live view is — stays visible.
            ++preview_drops;
            if (now - last_preview_summary >= std::chrono::seconds(1)) {
                last_preview_summary = now;
                const double stale_ms =
                    std::chrono::duration<double, std::milli>(now - last_preview_push)
                        .count();
                qWarning().nospace()
                    << "[grade] preview throttle active, dropped=" << preview_drops
                    << " stale_ms=" << QString::number(stale_ms, 'f', 0);
            }
            return;
        }
        last_preview_push = now;
        canvas::core::Track::Kind kind;
        std::size_t index = 0;
        canvas::core::Clip clip;
        if (!resolve_grade_target(kind, index, clip)) return;
        if (drag_begin) {
            // Color archive: the TRUE pre-change snapshot — frame, target clip,
            // and its grade before any of THIS interaction's previews mutated it.
            CANVAS_COLOR_LOG(
                "[grade] drag-begin kind=%d track=%zu clip=%llu frame=%lld "
                "prior=grade nodes=%zu edges=%zu has=%d state=%s",
                static_cast<int>(kind), static_cast<unsigned long long>(index),
                static_cast<unsigned long long>(clip.id),
                static_cast<long long>(mw.controller_.current_frame()),
                clip.grade.num_nodes(), clip.grade.edges().size(),
                clip.has_grade() ? 1 : 0,
                grade_state_digest(wheels->state(), curves->params()).toStdString().c_str());
        }
        canvas::core::grade_graph::GradeGraph g =
            make_grade_graph(wheels->state(), curves->params());
        g.change_seq = ++grade_seq;
        auto cmd = canvas::core::set_clip_grade(mw.project_->sequence, kind,
                index, clip.id, g);
        if (!cmd) return;
        cmd->redo(mw.project_->sequence);
        mw.refresh_timeline();
        mw.push_grade_snapshot();
        // Preview cadence trace: every ACCEPTED push (≈30/s while dragging),
        // with the drop count since the last accepted one. This is the log the
        // throttle fix is judged against — a healthy read is ~30 preview lines
        // a second and a monotone drop count that stays small per pause.
        qWarning().nospace()
            << "[grade] preview seq=" << g.change_seq
            << " t=" << canvas::core::log::epoch_ms()
            << " kind=" << static_cast<int>(kind)
            << " track=" << index << " clip=" << clip.id
            << (preview_drops
                    ? QStringLiteral(" dropped=%1").arg(preview_drops)
                    : QString())
            << " " << graph_census(g)
            << " " << grade_state_digest(wheels->state(), curves->params());
        CANVAS_COLOR_LOG(
            "[grade] preview seq=%llu kind=%d clip=%llu frame=%lld "
            "new=grade nodes=%zu edges=%zu state=%s",
            static_cast<unsigned long long>(g.change_seq), static_cast<int>(kind),
            static_cast<unsigned long long>(clip.id),
            static_cast<long long>(mw.controller_.current_frame()),
            g.num_nodes(), g.edges().size(),
            grade_state_digest(wheels->state(), curves->params()).toStdString().c_str());
        preview_drops = 0;
        last_preview_summary = now;
    };
    const auto commit_conn =
        QObject::connect(wheels, &ColorWheelsPanel::params_committed, &mw, commit_grade);
    const auto preview_conn =
        QObject::connect(wheels, &ColorWheelsPanel::params_preview, &mw, preview_grade);
    // Wiring probe: proves at runtime that BOTH signal->lambda connections were
    // actually registered on the live panel object (a silently-zero connection
    // here is the prime suspect when wheel drags log moves but never preview).
    CANVAS_COLOR_LOG(
        "[preview] wiring preview_conn=%d commit_conn=%d wheels=%p preview_throttle=%lldms",
        (preview_conn ? 1 : 0), (commit_conn ? 1 : 0),
        static_cast<void*>(wheels),
        static_cast<long long>(kGradePreviewThrottle.count()));
    qWarning().nospace()
        << "[grade] wiring preview_conn=" << (preview_conn ? 1 : 0)
        << " commit_conn=" << (commit_conn ? 1 : 0);
    // Reset-all is a page-level action: the wheels panel reverted itself, so
    // here we ALSO clear the Curves panel (the wheels panel can't see it) and
    // write a truly EMPTY grade — the clip goes fully ungraded and the decoder
    // stops baking even an identity LUT. One undo entry covers the whole reset.
    const auto reset_all_grades = [&mw, wheels, curves, resolve_grade_target] {
        wheels->set_state(canvas::core::colorsci::WheelPanelState{});
        curves->set_params(canvas::core::colorsci::CurveParams{});
        canvas::core::Track::Kind kind;
        std::size_t index = 0;
        canvas::core::Clip clip;
        if (!resolve_grade_target(kind, index, clip)) {
            qWarning().nospace()
                << "[grade] reset-all no target, panels cleared: no selection and "
                   "no video clip at playhead "
                << mw.controller_.current_frame();
            return;
        }
        canvas::core::grade_graph::GradeGraph g;
        g.change_seq = ++grade_seq;
        CANVAS_COLOR_LOG(
            "[grade] pre-change seq=%llu kind=%d track=%zu clip=%llu frame=%lld "
            "prior=grade nodes=%zu edges=%zu has=%d (reset-all)",
            static_cast<unsigned long long>(g.change_seq), static_cast<int>(kind),
            static_cast<unsigned long long>(index),
            static_cast<unsigned long long>(clip.id),
            static_cast<long long>(mw.controller_.current_frame()),
            clip.grade.num_nodes(), clip.grade.edges().size(), clip.has_grade() ? 1 : 0);
        auto cmd = canvas::core::set_clip_grade(mw.project_->sequence, kind, index,
                                                clip.id, g);
        if (!cmd) return;
        mw.undo_.record(std::move(cmd));
        qWarning().nospace()
            << "[grade] reset-all commit seq=" << g.change_seq
            << " t=" << canvas::core::log::epoch_ms()
            << " kind=" << static_cast<int>(kind)
            << " track=" << index << " clip=" << clip.id
            << " undo=" << mw.undo_.count() << " grade=EMPTY (ungraded)";
        CANVAS_COLOR_LOG(
            "[grade] reset-all seq=%llu kind=%d clip=%llu frame=%lld grade=EMPTY",
            static_cast<unsigned long long>(g.change_seq), static_cast<int>(kind),
            static_cast<unsigned long long>(clip.id),
            static_cast<long long>(mw.controller_.current_frame()));
        mw.has_unsaved_changes_ = true;
        mw.refresh_timeline();
        mw.push_grade_snapshot();
    };
    QObject::connect(wheels, &ColorWheelsPanel::reset_all_requested, &mw, reset_all_grades);
    QObject::connect(curves, &CurvesPanel::curves_committed, &mw, commit_grade);
    QObject::connect(curves, &CurvesPanel::curves_preview, &mw, preview_grade);

    // Page-toolbar toggles → panel visibility.
    // The media panel opens compact (~20% of the page) once, so the grading
    // tools at the bottom keep the width — afterwards it drags freely like the
    // edit-tab media pool and the size is preserved.
    auto left_panels_sized = std::make_shared<bool>(false);
    const auto show_left_panel = [&mw, color_tabs, gallery_btn, luts_btn,
                                  left_panels_sized](bool, int tab) {
        color_tabs->setCurrentIndex(tab);
        mw.color_left_dock_->setVisible(gallery_btn->isChecked() || luts_btn->isChecked());
        if (!*left_panels_sized && mw.color_left_dock_->isVisible()) {
            *left_panels_sized = true;
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
    const bool already_active = mw.color_active_;
    mw.color_active_ = true;
    // Always-on page trace: distinguishes "the user actually entered the Color
    // page" from the tonefield/ministrip lines that fire during startup dock
    // construction — the two look identical otherwise (see the 2026-09-09 log
    // rounds where no grade interaction appeared despite the wheels existing).
    qWarning() << "[page] enter color";
    // The scopes/curve-veil read CPU graded pixels; enable the decoder's
    // graded-preview feed for the Color page session (Edit-tab playback stays
    // on the pure GPU NV12 fast path).
    mw.controller_.set_cpu_graded_preview_enabled(true);
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
    // Last presented frame was produced with the feed OFF — step to the same
    // frame so the scopes immediately get a graded `a` to chew on. Skip on a
    // re-enter (already decoding with the feed on).
    if (!already_active && !mw.controller_.is_playing())
        mw.controller_.step(0);
}

void leave_color_page(MainWindow& mw) {
    if (!mw.color_active_) return;
    mw.color_active_ = false;
    qWarning() << "[page] leave color";
    mw.controller_.set_cpu_graded_preview_enabled(false);
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