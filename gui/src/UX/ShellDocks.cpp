#include "UX/MainWindow.hpp"
#include "ui_MainWindow.h"

#include "UX/InspectorAudio.hpp"
#include "UX/InspectorFile.hpp"
#include "UX/InspectorTransition.hpp"
#include "UX/InspectorVisual.hpp"

#include <QAbstractItemView>
#include <QButtonGroup>
#include <QColor>
#include <QDockWidget>
#include <QDoubleSpinBox>
#include <QFrame>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QPalette>
#include <QPoint>
#include <QPushButton>
#include <QScrollArea>
#include <QSize>
#include <QStackedWidget>
#include <QTabWidget>
#include <QToolButton>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QVariant>
#include <QVBoxLayout>
#include <QWidget>

#include <algorithm>

#include "UX/theme.hpp"
#include "Widgets/media_pool_widget.hpp"
#include "canvas/core/media/frame.hpp"
#include "canvas/core/media/video_decoder.hpp"

namespace canvas::gui {

void build_left_dock(MainWindow& mw) {
    auto* left_tabs = new QTabWidget(&mw);
    left_tabs->setObjectName(QStringLiteral("leftTabStrip"));
    left_tabs->setTabPosition(QTabWidget::North);
    left_tabs->setMinimumWidth(380);
    left_tabs->setDocumentMode(true);
    apply_theme_style(left_tabs, &left_tab_strip_style);

    auto* pool_tab = new QWidget(left_tabs);
    auto* pool_root_layout = new QVBoxLayout(pool_tab);
    pool_root_layout->setContentsMargins(8, 8, 8, 8);
    pool_root_layout->setSpacing(8);

    // Search row: filter field + Import, matching the reference panel top.
    auto* search_row = new QWidget(pool_tab);
    auto* search_layout = new QHBoxLayout(search_row);
    search_layout->setContentsMargins(0, 0, 0, 0);
    search_layout->setSpacing(6);
    auto* search = new QLineEdit(search_row);
    search->setObjectName(QStringLiteral("mediaSearch"));
    search->setPlaceholderText(MainWindow::tr("Search media…"));
    search->setClearButtonEnabled(true);
    search->addAction(icon("search"), QLineEdit::LeadingPosition);
    apply_theme_style(search, [] {
        const ThemeTokens& t = tokens();
        return QStringLiteral(
            "QLineEdit#mediaSearch { background-color: %1; border: 1px solid %2;"
            " border-radius: 8px; padding: 5px 10px; color: %3; font-size: 13px;}"
            "QLineEdit#mediaSearch:focus { border-color: %4; }"
            "QLineEdit#mediaSearch::placeholder { color: %5; }")
            .arg(css(t.surface_raised), css(t.border), css(t.ink), css(t.accent),
                 css(t.ink_faint));
    });
    search_layout->addWidget(search, 1);
    auto* import_btn = new QPushButton(MainWindow::tr("Import"), search_row);
    import_btn->setObjectName(QStringLiteral("mediaImport"));
    import_btn->setCursor(Qt::PointingHandCursor);
    import_btn->setFixedHeight(32);
    apply_theme_style(import_btn, [] {
        const ThemeTokens& t = tokens();
        return QStringLiteral(
            "QPushButton#mediaImport { background-color: %1; color: %2; border: none;"
            " border-radius: 8px; padding: 0 14px; font-size: 13px; font-weight: 550;}"
            "QPushButton#mediaImport:hover { background-color: %3; }"
            "QPushButton#mediaImport:pressed { background-color: %4; }"
            "QPushButton#mediaImport:focus { outline: none; }")
            .arg(css(t.accent), css(t.on_accent), css(t.accent_hover),
                 css(t.accent_press));
    });
    search_layout->addWidget(import_btn);
    pool_root_layout->addWidget(search_row);

    // Two-column body: bins | pool.
    auto* pool_body = new QWidget(pool_tab);
    auto* pool_body_layout = new QHBoxLayout(pool_body);
    pool_body_layout->setContentsMargins(0, 0, 0, 0);
    pool_body_layout->setSpacing(8);

    // Bins column.
    auto* bins_column = new QWidget(pool_body);
    bins_column->setFixedWidth(104);
    auto* bins_layout = new QVBoxLayout(bins_column);
    bins_layout->setContentsMargins(0, 0, 0, 0);
    bins_layout->setSpacing(0);
    auto* bins_label = new QLabel(MainWindow::tr("Bins"), bins_column);
    bins_label->setText(bins_label->text().toUpper());
    apply_theme_style(bins_label, [] {
        return QStringLiteral(
            "color: %1; font-size: 10px; font-weight: 600; letter-spacing: 0.08em;"
            " text-transform: uppercase; padding: 6px 9px 2px; background-color: transparent;")
            .arg(css(tokens().ink_muted));
    });
    auto* bin_tree = new QTreeWidget(bins_column);
    bin_tree->setObjectName(QStringLiteral("binTree"));
    bin_tree->setHeaderHidden(true);
    bin_tree->setRootIsDecorated(false);
    bin_tree->setColumnCount(2);
    bin_tree->setColumnWidth(0, 62);
    bin_tree->setColumnWidth(1, 26);
    bin_tree->header()->setStretchLastSection(false);
    bin_tree->setIconSize(QSize(16, 16));
    apply_theme_style(bin_tree, &bin_tree_style);
    {
        // Palette-driven surfaces so viewport/text/selection follow tokens
        // (kept alive across appearance switches via the re-apply hook).
        const auto retint_bins = [bin_tree] {
            const ThemeTokens& t = tokens();
            QPalette bp = bin_tree->palette();
            bp.setColor(QPalette::Base, t.surface);
            bp.setColor(QPalette::AlternateBase, t.surface);
            bp.setColor(QPalette::Window, t.surface);
            bp.setColor(QPalette::Text, t.ink);
            bp.setColor(QPalette::Highlight, t.state_selected);
            bp.setColor(QPalette::HighlightedText, t.ink);
            bin_tree->setPalette(bp);
            if (QWidget* vp = bin_tree->viewport()) {
                vp->setAutoFillBackground(true);
                QPalette vpp = vp->palette();
                vpp.setColor(QPalette::Base, t.surface);
                vpp.setColor(QPalette::Window, t.surface);
                vpp.setColor(QPalette::Text, t.ink);
                vpp.setColor(QPalette::Highlight, t.state_selected);
                vpp.setColor(QPalette::HighlightedText, t.ink);
                vp->setPalette(vpp);
                vp->setStyleSheet(QStringLiteral("background-color: %1;").arg(css(t.surface)));
            }
        };
        retint_bins();
        register_theme_reapply(retint_bins);
    }
    mw.bin_tree_ = bin_tree;
    mw.bin_tree_->setEditTriggers(QAbstractItemView::DoubleClicked | QAbstractItemView::EditKeyPressed |
                                  QAbstractItemView::SelectedClicked);
    mw.refresh_bin_tree();
    mw.bin_tree_->setContextMenuPolicy(Qt::CustomContextMenu);
    QObject::connect(mw.bin_tree_, &QTreeWidget::customContextMenuRequested, &mw,
            [&mw](const QPoint& pos) {
                QMenu menu;
                QTreeWidgetItem* item = mw.bin_tree_->itemAt(pos);
                menu.addAction(MainWindow::tr("New Bin"), &mw, [&mw]() {
                    mw.project_->bins.push_back("New Bin");
                    mw.refresh_bin_tree();
                    if (mw.bin_tree_->topLevelItemCount() > 0) {
                        QTreeWidgetItem* created = mw.bin_tree_->topLevelItem(mw.bin_tree_->topLevelItemCount() - 1);
                        mw.bin_tree_->setCurrentItem(created);
                        mw.bin_tree_->editItem(created, 0);
                    }
                    mw.has_unsaved_changes_ = true;
                });
                const int idx = item ? mw.bin_tree_->indexOfTopLevelItem(item) : -1;
                if (idx > 0) {
                    menu.addSeparator();
                    menu.addAction(MainWindow::tr("Delete Bin"), &mw, [&mw, idx]() {
                        if (idx <= 0 || idx >= mw.bin_tree_->topLevelItemCount()) return;
                        const QString name =
                            mw.bin_tree_->topLevelItem(idx)->data(0, Qt::UserRole).toString();
                        for (auto& m : mw.project_->media) {
                            if (QString::fromStdString(m.bin) == name) m.bin.clear();
                        }
                        mw.project_->bins.erase(
                            std::remove(mw.project_->bins.begin(), mw.project_->bins.end(), name.toStdString()),
                            mw.project_->bins.end());
                        if (mw.current_bin_ == name) mw.current_bin_.clear();
                        mw.refresh_bin_tree();
                        mw.refresh_media_pool();
                        mw.has_unsaved_changes_ = true;
                    });
                }
                menu.exec(mw.bin_tree_->viewport()->mapToGlobal(pos));
            });
    QObject::connect(mw.bin_tree_, &QTreeWidget::itemChanged, &mw, [&mw](QTreeWidgetItem* item, int) {
        if (!item || item->parent()) return;
        const QString old = item->data(0, Qt::UserRole).toString();
        const QString fresh = item->text(0).trimmed();
        if (fresh.isEmpty()) {
            item->setText(0, old.isEmpty() ? QStringLiteral("Master") : old);
            return;
        }
        if (!old.isEmpty() && old != fresh) {
            for (auto& m : mw.project_->media)
                if (QString::fromStdString(m.bin) == old) m.bin = fresh.toStdString();
            if (mw.current_bin_ == old) mw.current_bin_ = fresh;
        }
        item->setData(0, Qt::UserRole, fresh);
        mw.project_->bins.clear();
        for (int i = 1; i < mw.bin_tree_->topLevelItemCount(); ++i)
            mw.project_->bins.push_back(mw.bin_tree_->topLevelItem(i)->data(0, Qt::UserRole).toString().toStdString());
        mw.refresh_media_pool();
        mw.has_unsaved_changes_ = true;
    });
    bins_layout->addWidget(bins_label);
    bins_layout->addWidget(bin_tree, 1);

    // Grid column.
    auto* grid_column = new QWidget(pool_body);
    grid_column->setMinimumWidth(220);
    auto* grid_layout = new QVBoxLayout(grid_column);
    grid_layout->setContentsMargins(0, 0, 0, 0);
    grid_layout->setSpacing(0);
    QObject::connect(bin_tree, &QTreeWidget::currentItemChanged, &mw,
            [&mw](QTreeWidgetItem* cur, QTreeWidgetItem*) {
                const QString name = cur ? cur->data(0, Qt::UserRole).toString() : QString();
                mw.set_current_bin(cur && cur->data(0, Qt::UserRole).isValid() ? name : QString());
            });

    mw.media_pool_ = new MediaPoolWidget(grid_column);
    mw.media_pool_->setObjectName(QStringLiteral("mediaPool"));
    apply_theme_style(mw.media_pool_, &media_pool_style);
    mw.media_pool_->setContextMenuPolicy(Qt::CustomContextMenu);
    mw.media_pool_->setSpacing(6);
    QObject::connect(mw.media_pool_, &MediaPoolWidget::importRequested, &mw, &MainWindow::on_import_media);
    QObject::connect(mw.media_pool_, &MediaPoolWidget::deleteSelectedRequested, &mw,
                     &MainWindow::delete_selected_media);
    QObject::connect(mw.media_pool_, &MediaPoolWidget::deleteSelectedWithClipsRequested, &mw,
                     &MainWindow::delete_selected_media_and_clips);
    grid_layout->addWidget(mw.media_pool_, 1);

    QObject::connect(mw.media_pool_, &QListWidget::customContextMenuRequested, &mw,
            [&mw](const QPoint& pos) {
                QMenu menu;
                menu.addAction(MainWindow::tr("Import Media..."), &mw, &MainWindow::on_import_media);
                menu.addSeparator();
                menu.addAction(MainWindow::tr("Delete Selected Media"),
                               &mw, &MainWindow::delete_selected_media);
                menu.addSeparator();
                menu.addAction(MainWindow::tr("Create Bin with Selected Clips..."));
                menu.addAction(MainWindow::tr("New Bin"));
                menu.addSeparator();
                menu.addAction(MainWindow::tr("Auto Sync Audio..."));
                menu.addSeparator();
                menu.addAction(MainWindow::tr("Always Open in Thumbnail View"))->setCheckable(true);
                menu.exec(mw.media_pool_->viewport()->mapToGlobal(pos));
            });
    QObject::connect(mw.media_pool_, &QListWidget::itemDoubleClicked, &mw, [&mw](QListWidgetItem* item) {
        const QVariant v = item->data(Qt::UserRole);
        if (!v.isValid()) return;
        const int idx = static_cast<int>(v.toLongLong());
        if (idx < 0 || static_cast<std::size_t>(idx) >= mw.project_->media.size()) return;
        const auto& media = mw.project_->media[idx];
        mw.viewer_->set_mode(ViewerGL::ViewerMode::Source);
        canvas::core::VideoDecoder probe;
        std::string error;
        if (probe.open(media.path, &error)) {
            if (auto f = probe.seek_to_frame(0)) {
                auto rf = std::make_shared<canvas::core::RenderFrame>();
                rf->a = std::move(f);
                mw.viewer_->set_frame(std::move(rf));
                mw.status_->showMessage(MainWindow::tr("Source: %1 (%2x%3, %4fps)")
                                            .arg(QString::fromStdString(media.path))
                                            .arg(probe.width())
                                            .arg(probe.height())
                                            .arg(probe.frame_rate(), 0, 'g', 3),
                                        4000);
            }
        }
    });

    pool_body_layout->addWidget(bins_column);
    pool_body_layout->addWidget(grid_column, 1);
    pool_root_layout->addWidget(pool_body, 1);

    // Search filters the current bin's pool by clip name.
    QObject::connect(search, &QLineEdit::textChanged, &mw, [&mw](const QString& needle) {
        if (!mw.media_pool_) return;
        for (int i = 0; i < mw.media_pool_->count(); ++i) {
            QListWidgetItem* it = mw.media_pool_->item(i);
            it->setHidden(needle.isEmpty() ||
                          !it->text().contains(needle, Qt::CaseInsensitive));
        }
    });
    QObject::connect(import_btn, &QPushButton::clicked, &mw, &MainWindow::on_import_media);

    left_tabs->addTab(pool_tab, MainWindow::tr("Media Pool"));

    for (const char* tab_name : {"Sync Bin", "Transitions", "Titles", "Effects", "Index", "Sound Library", "Keyframes"}) {
        auto* placeholder = new QLabel(MainWindow::tr("%1 — placeholder").arg(MainWindow::tr(tab_name)), left_tabs);
        placeholder->setAlignment(Qt::AlignCenter);
        apply_theme_style(placeholder, [] {
            return QStringLiteral("color: %1;").arg(css(tokens().ink_faint));
        });
        left_tabs->addTab(placeholder, MainWindow::tr(tab_name));
    }

    mw.media_dock_ = mw.ui->mediaDock;
    mw.media_dock_->setObjectName(QStringLiteral("mediaDock"));
    // The dock's own background carries the mint workspace glow; the glass card
    // below floats on it (mirror of the viewer column's viewerFrame).
    apply_theme_style(mw.media_dock_, &dock_glow_style);
    auto* media_title = new QWidget(mw.media_dock_);
    media_title->setObjectName(QStringLiteral("mediaDockTitle"));
    apply_theme_style(media_title, [] {
        return QStringLiteral("QWidget#mediaDockTitle { background: transparent;"
                              " border: none; }");
    });
    mw.media_dock_->setTitleBarWidget(media_title);
    // Floating glass card wrapping the tab strip, so the tab strip + its panes
    // read as one rounded panel hovering over the workspace glow.
    auto* media_glass = new QFrame(&mw);
    media_glass->setObjectName(QStringLiteral("dockGlassCard"));
    apply_theme_style(media_glass, &dock_panel_style);
    auto* media_glass_layout = new QVBoxLayout(media_glass);
    media_glass_layout->setContentsMargins(0, 0, 0, 0);
    media_glass_layout->setSpacing(0);
    media_glass_layout->addWidget(left_tabs);
    mw.media_dock_->setWidget(media_glass);
    mw.media_dock_->setFeatures(QDockWidget::DockWidgetMovable | QDockWidget::DockWidgetFloatable);
}

void build_inspector_dock(MainWindow& mw) {
    mw.inspector_dock_ = mw.ui->inspectorDock;
    mw.inspector_dock_->setObjectName(QStringLiteral("inspectorDock"));
    apply_theme_style(mw.inspector_dock_, &dock_glow_style);
    auto* inspector_title = new QWidget(mw.inspector_dock_);
    inspector_title->setObjectName(QStringLiteral("inspectorDockTitle"));
    apply_theme_style(inspector_title, [] {
        return QStringLiteral("QWidget#inspectorDockTitle { background: transparent;"
                              " border: none; }");
    });
    mw.inspector_dock_->setTitleBarWidget(inspector_title);
    mw.inspector_dock_->setMinimumWidth(320);
    mw.inspector_dock_->setFeatures(QDockWidget::DockWidgetMovable | QDockWidget::DockWidgetFloatable);

    auto* inspector_body = new QWidget(mw.inspector_dock_);
    inspector_body->setObjectName(QStringLiteral("dockGlassCard"));
    apply_theme_style(inspector_body, &dock_panel_style);
    auto* inspector_outer = new QVBoxLayout(inspector_body);
    inspector_outer->setContentsMargins(0, 0, 0, 0);
    inspector_outer->setSpacing(0);

    auto* mode_row = new QWidget(inspector_body);
    apply_theme_style(mode_row, &inspector_tab_track_style);
    auto* mode_row_layout = new QHBoxLayout(mode_row);
    mode_row_layout->setContentsMargins(4, 4, 4, 4);
    mode_row_layout->setSpacing(2);
    struct ModePill { const char* label; const char* icon_name; };
    const ModePill modes[] = {
        {"Video", "settings"}, {"Audio", "volume"}, {"Effects", "mode"},
        {"Transition", "transition"}, {"Image", "viewport"}, {"File", "film-strip"},
    };
    auto* mode_group = new QButtonGroup(mode_row);
    mode_group->setExclusive(true);
    std::vector<QToolButton*> mode_buttons;
    for (const auto& m : modes) {
        auto* b = new QToolButton(mode_row);
        const bool is_video = qstrcmp(m.label, "Video") == 0;
        b->setText(MainWindow::tr(m.label));
        b->setIcon(icon(m.icon_name));
        b->setIconSize(QSize(14, 14));
        b->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
        b->setCheckable(true);
        b->setChecked(is_video);
        b->setAutoRaise(true);
        apply_theme_style(b, &inspector_tab_style);
        b->setToolTip(MainWindow::tr(m.label));
        // Allow the pill to shrink below its text width so six mode buttons
        // fit comfortably at any DPI scale and dock width.  A tooltip makes
        // the truncated label discoverable.
        b->setMinimumWidth(1);
        b->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        mode_group->addButton(b);
        mode_row_layout->addWidget(b);
        mode_buttons.push_back(b);
    }
    inspector_outer->addWidget(mode_row);

    auto* scroll = new QScrollArea(inspector_body);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);

    // One category stack per mode tab; the tabs switch which one is on top.
    // "Audio" carries the working per-clip mix controls; the other modes keep
    // their reference layouts until their properties are wired to the model.
    auto* stack = new QStackedWidget(scroll);
    const int video_tab_index = 0;
    const int audio_tab_index = 1;
    const int transition_tab_index = 3;

    // --- Video page ---------------------------------------------------------
    auto* video_page = new QWidget(stack);
    auto* video_layout = new QVBoxLayout(video_page);
    // Vertical gutters so the floating category cards sit off the page edges
    // and each card has clear separation from the next.
    video_layout->setContentsMargins(0, 4, 0, 8);
    video_layout->setSpacing(0);

    // Video tab's property categories (Transform/Composite + the reference
    // placeholders) all build in InspectorVisual.cpp (splitplan refactor); the
    // widget handles + selection wiring live there too.
    build_inspector_visual(mw, video_layout);
    video_layout->addStretch(1);
    stack->addWidget(video_page);

    // --- Audio page ---------------------------------------------------------
    // The full Audio tab (Volume/Pan, Pitch, Speed Change, Equalizer + the AI
    // placeholder sections) builds in InspectorAudio.cpp (splitplan refactor).
    // Its volume/pan spins subscribe to MainWindow::apply_inspector_audio and
    // feed the legacy member pointers, so the pre-split commit path still works.
    auto* audio_page = new QWidget(stack);
    auto* audio_layout = new QVBoxLayout(audio_page);
    audio_layout->setContentsMargins(0, 4, 0, 8);
    audio_layout->setSpacing(0);
    build_inspector_audio(mw, audio_layout, mode_buttons[audio_tab_index]);
    audio_layout->addStretch(1);
    stack->addWidget(audio_page);

    // --- Effects / Image pages (placeholders for now) -----------------------
    // Pages are added to the stack in the SAME order as the mode pills above
    // (Video, Audio, Effects, Transition, Image, File); the toggled handler
    // switches by pill index, so a misplaced page surfaces under the wrong tab.
    auto* effects_page = new QWidget(stack);
    {
        auto* page_layout = new QVBoxLayout(effects_page);
        page_layout->setContentsMargins(10, 10, 10, 10);
        page_layout->setSpacing(0);
        auto* hint = new QLabel(MainWindow::tr("Effects properties — not available yet."),
                                effects_page);
        hint->setContentsMargins(10, 10, 10, 10);
        apply_theme_style(hint, [] {
            return QStringLiteral("color: %1; font-size: 11px;").arg(css(tokens().ink_faint));
        });
        hint->setWordWrap(true);
        page_layout->addWidget(hint);
        page_layout->addStretch(1);
    }
    stack->addWidget(effects_page);  // index 2

    // --- Transition page ----------------------------------------------------
    // Start/End sub-tabs + Video/Audio categories (InspectorTransition.cpp).
    // Only active while a transition bubble is selected on the timeline.
    auto* transition_page = new QWidget(stack);
    auto* transition_layout = new QVBoxLayout(transition_page);
    transition_layout->setContentsMargins(0, 4, 0, 8);
    transition_layout->setSpacing(0);
    build_inspector_transition(mw, transition_layout, mode_buttons[transition_tab_index]);
    stack->addWidget(transition_page);  // index 3

    auto* image_page = new QWidget(stack);
    {
        auto* page_layout = new QVBoxLayout(image_page);
        page_layout->setContentsMargins(10, 10, 10, 10);
        page_layout->setSpacing(0);
        auto* hint = new QLabel(MainWindow::tr("Image properties — not available yet."),
                                image_page);
        hint->setContentsMargins(10, 10, 10, 10);
        apply_theme_style(hint, [] {
            return QStringLiteral("color: %1; font-size: 11px;").arg(css(tokens().ink_faint));
        });
        hint->setWordWrap(true);
        page_layout->addWidget(hint);
        page_layout->addStretch(1);
    }
    stack->addWidget(image_page);  // index 4

    // --- File page ----------------------------------------------------------
    // Read-only source header info + fully-wired metadata (InspectorFile.cpp).
    auto* file_page = new QWidget(stack);
    auto* file_layout = new QVBoxLayout(file_page);
    file_layout->setContentsMargins(0, 4, 0, 8);
    file_layout->setSpacing(0);
    build_inspector_file(mw, file_layout);
    stack->addWidget(file_page);

    for (std::size_t i = 0; i < mode_buttons.size(); ++i) {
        const int idx = static_cast<int>(i);
        QObject::connect(mode_buttons[idx], &QToolButton::toggled, stack, [stack, idx](bool on) {
            if (on) stack->setCurrentIndex(idx);
        });
    }
    stack->setCurrentIndex(video_tab_index);

    scroll->setWidget(stack);
    inspector_outer->addWidget(scroll, 1);

    mw.inspector_dock_->setWidget(inspector_body);
    mw.inspector_dock_->hide();
    QObject::connect(mw.inspector_toggle_action_, &QAction::toggled, mw.inspector_dock_, &QDockWidget::setVisible);
    // The top-bar Inspector button is created later (build_top_bar) — it connects
    // back to this action/dock there, where all three objects are already alive.
}

}  // namespace canvas::gui