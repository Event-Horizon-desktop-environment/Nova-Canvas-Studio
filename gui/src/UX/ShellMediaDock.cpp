#include "UX/MainWindow.hpp"
#include "ui_MainWindow.h"

#include "UX/theme.hpp"
#include "UX/empty_state.hpp"
#include "Widgets/media_pool_widget.hpp"

#include <QAbstractItemView>
#include <QDockWidget>
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
#include <QSize>
#include <QTabWidget>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QVariant>
#include <QVBoxLayout>
#include <QWidget>

#include <algorithm>

#include "canvas/core/media/frame.hpp"
#include "canvas/core/media/video_decoder.hpp"

namespace canvas::gui {

// The left dock: Bins column + Media Pool grid, in a Resolve-style tab strip
// with a floating glass card and a collapse sliver at the dock's outer edge.
// Split out of the old ShellDocks.cpp so the media-pool chrome can move
// independently of the Inspector (ShellInspectorDock.cpp).
void build_left_dock(MainWindow& mw) {
    auto* left_tabs = new QTabWidget(&mw);
    left_tabs->setObjectName(QStringLiteral("leftTabStrip"));
    left_tabs->setTabPosition(QTabWidget::North);
    left_tabs->setMinimumWidth(384);
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
    search_layout->setSpacing(8);
    auto* search = new QLineEdit(search_row);
    search->setObjectName(QStringLiteral("mediaSearch"));
    search->setPlaceholderText(MainWindow::tr("Search media…"));
    search->setClearButtonEnabled(true);
    search->addAction(icon("search"), QLineEdit::LeadingPosition);
    apply_theme_style(search, [] {
        const ThemeTokens& t = tokens();
        return QStringLiteral(
            "QLineEdit#mediaSearch { background-color: %1; border: 1px solid %2;"
            " border-radius: 8px; padding: 6px 12px; color: %3; font-size: 14px;}"
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
            " border-radius: 8px; padding: 0 16px; font-size: 14px; font-weight: 550;}"
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
                apply_rounded_menu(&menu);
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
    mw.media_pool_->setSpacing(8);
    QObject::connect(mw.media_pool_, &MediaPoolWidget::importRequested, &mw, &MainWindow::on_import_media);
    QObject::connect(mw.media_pool_, &MediaPoolWidget::deleteSelectedRequested, &mw,
                     &MainWindow::delete_selected_media);
    QObject::connect(mw.media_pool_, &MediaPoolWidget::deleteSelectedWithClipsRequested, &mw,
                     &MainWindow::delete_selected_media_and_clips);
    grid_layout->addWidget(mw.media_pool_, 1);

    QObject::connect(mw.media_pool_, &QListWidget::customContextMenuRequested, &mw,
            [&mw](const QPoint& pos) {
                QMenu menu;
                apply_rounded_menu(&menu);
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

    const struct { const char* tab; const char* icon; } placeholders[] = {
        {"Sync Bin", "sync_lock"},       {"Transitions", "transition"},
        {"Titles", "edit"},              {"Effects", "effects"},
        {"Index", "search"},             {"Sound Library", "volume"},
        {"Keyframes", "mode"},
    };
    for (const auto& p : placeholders) {
        auto* page = build_empty_state(left_tabs, p.icon,
                                       MainWindow::tr(p.tab),
                                       MainWindow::tr("This panel is coming in a future update."));
        left_tabs->addTab(page, MainWindow::tr(p.tab));
    }

    mw.media_dock_ = mw.ui->mediaDock;
    mw.media_dock_->setObjectName(QStringLiteral("mediaDock"));
    // The dock backdrop paints the flat workspace surface; the glass card below
    // floats on it (mirror of the viewer column's viewerFrame).
    apply_theme_style(mw.media_dock_, &dock_glow_style);
    auto* media_title = new QWidget(mw.media_dock_);
    media_title->setObjectName(QStringLiteral("mediaDockTitle"));
    apply_theme_style(media_title, [] {
        return QStringLiteral("QWidget#mediaDockTitle { background: transparent;"
                              " border: none; }");
    });
    mw.media_dock_->setTitleBarWidget(media_title);

    // Edge-to-edge panel wrapping the tab strip: the dock content is a flat
    // square well flush against the workspace surface — no float, no shadow.
    auto* media_glass = new QFrame(&mw);
    media_glass->setObjectName(QStringLiteral("dockGlassCard"));
    apply_theme_style(media_glass, &dock_panel_style);
    auto* media_glass_layout = new QVBoxLayout(media_glass);
    media_glass_layout->setContentsMargins(0, 0, 0, 0);
    media_glass_layout->setSpacing(0);
    media_glass_layout->addWidget(left_tabs);

    // The collapse/expand toggle now lives on the top status bar (ShellCenter,
    // just left of the Dual-Viewer button) so the dock edge stays clean. The
    // pool panel keeps its flat full-width glass below the tab strip; the
    // same QAbstractButton-style roundtrips mw.media_dock_ corner flips.
    mw.media_dock_->setWidget(media_glass);
    mw.media_dock_->setFeatures(QDockWidget::DockWidgetMovable | QDockWidget::DockWidgetFloatable);
}

}  // namespace canvas::gui