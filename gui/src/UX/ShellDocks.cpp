#include "UX/MainWindow.hpp"
#include "ui_MainWindow.h"

#include <QAbstractItemView>
#include <QColor>
#include <QComboBox>
#include <QDockWidget>
#include <QDoubleSpinBox>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QMenu>
#include <QPalette>
#include <QPoint>
#include <QScrollArea>
#include <QSize>
#include <QSlider>
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

namespace {

// ------------------------------------------------------------------------
// InspectorCategory — one collapsible property group, matching the Resolve
// Inspector convention (ux.md §3): enable dot | title | chevron | reset icon
// in the header, individual property rows in the body.
// ------------------------------------------------------------------------
class InspectorCategory : public QWidget {
public:
    InspectorCategory(const QString& title, bool expanded, QWidget* parent = nullptr)
        : QWidget(parent) {
        auto* outer = new QVBoxLayout(this);
        outer->setContentsMargins(0, 0, 0, 0);
        outer->setSpacing(0);

        header_ = new QToolButton(this);
        header_->setStyleSheet(inspector_category_header_style());
        header_->setToolButtonStyle(Qt::ToolButtonTextOnly);
        header_->setCheckable(true);
        header_->setChecked(expanded);
        header_->setArrowType(expanded ? Qt::DownArrow : Qt::RightArrow);
        header_->setText(QStringLiteral("  \u25CF  ") + title);

        auto* reset = new QToolButton(this);
        reset->setIcon(icon("reset"));
        reset->setIconSize(QSize(14, 14));
        reset->setAutoRaise(true);
        reset->setToolTip(tr("Reset to default"));

        auto* header_row = new QWidget(this);
        auto* header_layout = new QHBoxLayout(header_row);
        header_layout->setContentsMargins(0, 0, 4, 0);
        header_layout->setSpacing(0);
        header_layout->addWidget(header_, 1);
        header_layout->addWidget(reset);
        header_row->setStyleSheet(QStringLiteral("background-color: #1A1D27; border-bottom: 1px solid #232833;"));

        body_ = new QWidget(this);
        body_->setStyleSheet(inspector_body_style());
        body_layout_ = new QVBoxLayout(body_);
        body_layout_->setContentsMargins(10, 8, 10, 10);
        body_layout_->setSpacing(8);
        body_->setVisible(expanded);

        outer->addWidget(header_row);
        outer->addWidget(body_);

        connect(header_, &QToolButton::toggled, this, [this](bool on) {
            header_->setArrowType(on ? Qt::DownArrow : Qt::RightArrow);
            body_->setVisible(on);
        });
    }

    QVBoxLayout* body_layout() { return body_layout_; }

private:
    QToolButton* header_ = nullptr;
    QWidget* body_ = nullptr;
    QVBoxLayout* body_layout_ = nullptr;
};

// One property row: label | field | optional per-property reset icon (ux.md §3, §13).
void add_property_row(QVBoxLayout* body, const QString& label, QWidget* field, bool with_reset = true) {
    auto* row = new QHBoxLayout;
    row->setSpacing(6);
    auto* lbl = new QLabel(label);
    lbl->setMinimumWidth(78);
    lbl->setStyleSheet(QStringLiteral("color: #9AA0B0; font-size: 11px;"));
    row->addWidget(lbl);
    row->addWidget(field, 1);
    if (with_reset) {
        auto* reset = new QToolButton;
        reset->setIcon(icon("reset"));
        reset->setIconSize(QSize(14, 14));
        reset->setAutoRaise(true);
        reset->setFixedWidth(18);
        row->addWidget(reset);
    }
    body->addLayout(row);
}

QDoubleSpinBox* make_numeric(double lo, double hi, double val, QWidget* parent) {
    auto* s = new QDoubleSpinBox(parent);
    s->setRange(lo, hi);
    s->setValue(val);
    s->setDecimals(3);
    s->setMaximumWidth(90);
    return s;
}

}  // namespace

void build_left_dock(MainWindow& mw) {
    auto* left_tabs = new QTabWidget(&mw);
    left_tabs->setObjectName(QStringLiteral("leftTabStrip"));
    left_tabs->setTabPosition(QTabWidget::North);
    left_tabs->setMinimumWidth(380);
    left_tabs->setMaximumWidth(560);

    auto* pool_tab = new QWidget(left_tabs);
    auto* pool_root_layout = new QHBoxLayout(pool_tab);
    pool_root_layout->setContentsMargins(0, 0, 0, 0);
    pool_root_layout->setSpacing(0);

    // Bins column.
    auto* bins_column = new QWidget(pool_tab);
    auto* bins_layout = new QVBoxLayout(bins_column);
    bins_layout->setContentsMargins(0, 0, 0, 0);
    bins_layout->setSpacing(0);
    auto* bins_label = new QLabel(MainWindow::tr("Bins"), bins_column);
    bins_label->setStyleSheet(QStringLiteral("color: #9AA0B0; font-size: 10px; padding: 4px 6px; background-color: #11131A;"));
    auto* bin_tree = new QTreeWidget(bins_column);
    bin_tree->setObjectName(QStringLiteral("binTree"));
    bin_tree->setHeaderHidden(true);
    bin_tree->setRootIsDecorated(false);
    bin_tree->setIconSize(QSize(28, 28));
    bin_tree->setStyleSheet(bin_tree_style());
    {
        QPalette bp = bin_tree->palette();
        bp.setColor(QPalette::Base, QColor(QStringLiteral("#11131A")));
        bp.setColor(QPalette::AlternateBase, QColor(QStringLiteral("#11131A")));
        bp.setColor(QPalette::Window, QColor(QStringLiteral("#11131A")));
        bp.setColor(QPalette::Text, QColor(QStringLiteral("#E8EAF0")));
        bp.setColor(QPalette::Highlight, QColor(255, 255, 255, 30));
        bp.setColor(QPalette::HighlightedText, QColor(QStringLiteral("#E8EAF0")));
        bin_tree->setPalette(bp);
        if (auto* vp = bin_tree->viewport()) {
            vp->setAutoFillBackground(true);
            QPalette vpp = vp->palette();
            vpp.setColor(QPalette::Base, QColor(QStringLiteral("#11131A")));
            vpp.setColor(QPalette::Window, QColor(QStringLiteral("#11131A")));
            vpp.setColor(QPalette::Text, QColor(QStringLiteral("#E8EAF0")));
            vpp.setColor(QPalette::Highlight, QColor(255, 255, 255, 30));
            vpp.setColor(QPalette::HighlightedText, QColor(QStringLiteral("#E8EAF0")));
            vp->setPalette(vpp);
            vp->setStyleSheet(QStringLiteral("background-color: #11131A;"));
        }
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
    bins_column->setFixedWidth(120);

    // Grid column.
    auto* grid_column = new QWidget(pool_tab);
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
    mw.media_pool_->setStyleSheet(media_pool_style());
    mw.media_pool_->setContextMenuPolicy(Qt::CustomContextMenu);
    mw.media_pool_->setSpacing(6);
    QObject::connect(mw.media_pool_, &MediaPoolWidget::importRequested, &mw, &MainWindow::on_import_media);
    grid_layout->addWidget(mw.media_pool_, 1);

    QObject::connect(mw.media_pool_, &QListWidget::customContextMenuRequested, &mw,
            [&mw](const QPoint& pos) {
                QMenu menu;
                menu.addAction(MainWindow::tr("Import Media..."), &mw, &MainWindow::on_import_media);
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

    pool_root_layout->addWidget(bins_column);
    pool_root_layout->addWidget(grid_column, 1);
    left_tabs->addTab(pool_tab, MainWindow::tr("Media Pool"));

    for (const char* tab_name : {"Sync Bin", "Transitions", "Titles", "Effects", "Index", "Sound Library", "Keyframes"}) {
        auto* placeholder = new QLabel(MainWindow::tr("%1 — placeholder").arg(MainWindow::tr(tab_name)), left_tabs);
        placeholder->setAlignment(Qt::AlignCenter);
        placeholder->setStyleSheet(QStringLiteral("color: #5F6577;"));
        left_tabs->addTab(placeholder, MainWindow::tr(tab_name));
    }

    mw.media_dock_ = mw.ui->mediaDock;
    mw.media_dock_->setObjectName(QStringLiteral("mediaDock"));
    auto* media_title = new QWidget(mw.media_dock_);
    media_title->setObjectName(QStringLiteral("mediaDockTitle"));
    media_title->    setStyleSheet(QStringLiteral("background-color: #1A1D27;"));
    mw.media_dock_->setTitleBarWidget(media_title);
    mw.media_dock_->setWidget(left_tabs);
    mw.media_dock_->setFeatures(QDockWidget::DockWidgetMovable | QDockWidget::DockWidgetFloatable);
}

void build_inspector_dock(MainWindow& mw) {
    mw.inspector_dock_ = mw.ui->inspectorDock;
    mw.inspector_dock_->setObjectName(QStringLiteral("inspectorDock"));
    auto* inspector_title = new QWidget(mw.inspector_dock_);
    inspector_title->setObjectName(QStringLiteral("inspectorDockTitle"));
    inspector_title->    setStyleSheet(QStringLiteral("background-color: #1A1D27;"));
    mw.inspector_dock_->setTitleBarWidget(inspector_title);
    mw.inspector_dock_->setMinimumWidth(300);
    mw.inspector_dock_->setMaximumWidth(400);
    mw.inspector_dock_->setFeatures(QDockWidget::DockWidgetMovable | QDockWidget::DockWidgetFloatable);

    auto* inspector_body = new QWidget(mw.inspector_dock_);
    inspector_body->setStyleSheet(QStringLiteral("background-color: #141A21;"));
    auto* inspector_outer = new QVBoxLayout(inspector_body);
    inspector_outer->setContentsMargins(0, 0, 0, 0);
    inspector_outer->setSpacing(0);

    auto* mode_row = new QWidget(inspector_body);
    auto* mode_row_layout = new QHBoxLayout(mode_row);
    mode_row_layout->setContentsMargins(6, 6, 6, 6);
    mode_row_layout->setSpacing(2);
    const char* modes[] = {"Video", "Audio", "Effects", "Transition", "Image", "File"};
    for (const char* m : modes) {
        auto* b = new QToolButton(mode_row);
        const bool is_video = qstrcmp(m, "Video") == 0;
        b->setText(MainWindow::tr(m));
        b->setCheckable(true);
        b->setChecked(is_video);
        b->setAutoRaise(true);
        b->setStyleSheet(page_pill_style());
        mode_row_layout->addWidget(b);
    }
    inspector_outer->addWidget(mode_row);

    auto* scroll = new QScrollArea(inspector_body);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    auto* categories_host = new QWidget(scroll);
    auto* categories_layout = new QVBoxLayout(categories_host);
    categories_layout->setContentsMargins(0, 0, 0, 0);
    categories_layout->setSpacing(0);

    // Transform — expanded by default (§3, observed category order).
    auto* transform = new InspectorCategory(MainWindow::tr("Transform"), true, categories_host);
    auto* zoom_row = new QWidget(categories_host);
    auto* zoom_row_layout = new QHBoxLayout(zoom_row);
    zoom_row_layout->setContentsMargins(0, 0, 0, 0);
    zoom_row_layout->setSpacing(4);
    zoom_row_layout->addWidget(make_numeric(0.0, 10.0, 1.0, zoom_row));
    auto* chain = new QToolButton(zoom_row);
    chain->setIcon(icon("chain"));
    chain->setIconSize(QSize(14, 14));
    chain->setCheckable(true);
    chain->setChecked(true);
    chain->setToolTip(MainWindow::tr("Link X and Y"));
    zoom_row_layout->addWidget(chain);
    zoom_row_layout->addWidget(make_numeric(0.0, 10.0, 1.0, zoom_row));
    add_property_row(transform->body_layout(), MainWindow::tr("Zoom"), zoom_row);

    auto* pos_row = new QWidget(categories_host);
    auto* pos_row_layout = new QHBoxLayout(pos_row);
    pos_row_layout->setContentsMargins(0, 0, 0, 0);
    pos_row_layout->setSpacing(4);
    pos_row_layout->addWidget(make_numeric(-4096.0, 4096.0, 0.0, pos_row));
    pos_row_layout->addWidget(make_numeric(-4096.0, 4096.0, 0.0, pos_row));
    add_property_row(transform->body_layout(), MainWindow::tr("Position"), pos_row);

    add_property_row(transform->body_layout(), MainWindow::tr("Rotation Angle"), make_numeric(-360.0, 360.0, 0.0, categories_host));

    auto* anchor_row = new QWidget(categories_host);
    auto* anchor_row_layout = new QHBoxLayout(anchor_row);
    anchor_row_layout->setContentsMargins(0, 0, 0, 0);
    anchor_row_layout->setSpacing(4);
    anchor_row_layout->addWidget(make_numeric(-4096.0, 4096.0, 0.0, anchor_row));
    anchor_row_layout->addWidget(make_numeric(-4096.0, 4096.0, 0.0, anchor_row));
    add_property_row(transform->body_layout(), MainWindow::tr("Anchor Point"), anchor_row);

    add_property_row(transform->body_layout(), MainWindow::tr("Pitch"), make_numeric(-180.0, 180.0, 0.0, categories_host));
    add_property_row(transform->body_layout(), MainWindow::tr("Yaw"), make_numeric(-180.0, 180.0, 0.0, categories_host));

    auto* flip_row = new QWidget(categories_host);
    auto* flip_row_layout = new QHBoxLayout(flip_row);
    flip_row_layout->setContentsMargins(0, 0, 0, 0);
    flip_row_layout->setSpacing(4);
    auto* flip_h = new QToolButton(flip_row);
    flip_h->setIcon(icon("flip_h"));
    flip_h->setIconSize(QSize(14, 14));
    flip_h->setCheckable(true);
    flip_h->setToolTip(MainWindow::tr("Flip Horizontal"));
    auto* flip_v = new QToolButton(flip_row);
    flip_v->setIcon(icon("flip_v"));
    flip_v->setIconSize(QSize(14, 14));
    flip_v->setCheckable(true);
    flip_v->setToolTip(MainWindow::tr("Flip Vertical"));
    flip_row_layout->addWidget(flip_h);
    flip_row_layout->addWidget(flip_v);
    flip_row_layout->addStretch(1);
    add_property_row(transform->body_layout(), MainWindow::tr("Flip"), flip_row, /*with_reset=*/false);
    categories_layout->addWidget(transform);

    categories_layout->addWidget(new InspectorCategory(MainWindow::tr("AI Smart Reframe"), false, categories_host));
    categories_layout->addWidget(new InspectorCategory(MainWindow::tr("Cropping"), false, categories_host));
    categories_layout->addWidget(new InspectorCategory(MainWindow::tr("Dynamic Zoom"), false, categories_host));

    auto* composite = new InspectorCategory(MainWindow::tr("Composite"), false, categories_host);
    auto* blend_mode = new QComboBox(categories_host);
    blend_mode->addItems({MainWindow::tr("Normal"), MainWindow::tr("Add"), MainWindow::tr("Multiply"), MainWindow::tr("Screen"), MainWindow::tr("Overlay")});
    add_property_row(composite->body_layout(), MainWindow::tr("Composite Mode"), blend_mode);
    auto* opacity_row = new QWidget(categories_host);
    auto* opacity_row_layout = new QHBoxLayout(opacity_row);
    opacity_row_layout->setContentsMargins(0, 0, 0, 0);
    opacity_row_layout->setSpacing(4);
    auto* opacity_slider = new QSlider(Qt::Horizontal, opacity_row);
    opacity_slider->setRange(0, 100);
    opacity_slider->setValue(100);
    auto* opacity_value = new QLabel(QStringLiteral("100.00"), opacity_row);
    opacity_value->setStyleSheet(QStringLiteral("color: #E8EAF0; font-size: 11px;"));
    QObject::connect(opacity_slider, &QSlider::valueChanged, &mw, [opacity_value](int v) {
        opacity_value->setText(QString::number(v) + QStringLiteral(".00"));
    });
    opacity_row_layout->addWidget(opacity_slider, 1);
    opacity_row_layout->addWidget(opacity_value);
    add_property_row(composite->body_layout(), MainWindow::tr("Opacity"), opacity_row);
    categories_layout->addWidget(composite);

    for (const char* name : {"Speed Change", "Stabilization", "Lens Correction", "Retime and Scaling", "AI Super Scale"}) {
        categories_layout->addWidget(new InspectorCategory(MainWindow::tr(name), false, categories_host));
    }
    categories_layout->addStretch(1);

    scroll->setWidget(categories_host);
    inspector_outer->addWidget(scroll, 1);

    mw.inspector_dock_->setWidget(inspector_body);
    mw.inspector_dock_->hide();
    QObject::connect(mw.inspector_toggle_action_, &QAction::toggled, mw.inspector_dock_, &QDockWidget::setVisible);
    QObject::connect(mw.inspector_toggle_action_, &QAction::toggled, mw.inspector_top_btn_, &QToolButton::setChecked);
    QObject::connect(mw.inspector_top_btn_, &QToolButton::toggled, &mw,
            [&mw](bool on) {
                mw.inspector_toggle_action_->setChecked(on);
                if (mw.inspector_dock_) mw.inspector_dock_->setVisible(on);
            });
}

}  // namespace canvas::gui