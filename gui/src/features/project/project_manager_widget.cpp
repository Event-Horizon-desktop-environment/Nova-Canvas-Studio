// DaVinci Resolve-style Project Manager. The widget is pure view + grid
// logic: it reads the recent-projects list, paints Resolve-style cards (including
// the leading "+ New Project" tile), and asks MainWindow to decode card
// thumbnails through the shared ThumbnailService (bridge wired in
// MainWindow::enter_project_manager). ProjectManagerWindow wraps the page in a
// floating top-level window; enter/leave is defined here too so the whole seam
// lives with the widget instead of leaking into the action files.

#include "features/project/project_manager_widget.hpp"

#include "UX/MainWindow.hpp"
#include "UX/theme.hpp"

#include "canvas/core/project/project.hpp"

#include "features/thumbnails/thumbnail_service.hpp"

#include <QAction>
#include <QApplication>
#include <QCloseEvent>
#include <QComboBox>
#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QFileInfo>
#include <QFont>
#include <QFontMetricsF>
#include <QHBoxLayout>
#include <QImage>
#include <QLabel>
#include <QLineEdit>
#include <QLinearGradient>
#include <QListWidget>
#include <QMenu>
#include <QPainter>
#include <QPainterPath>
#include <QPen>
#include <QPixmap>
#include <QPushButton>
#include <QScreen>
#include <QSettings>
#include <QShortcut>
#include <QSignalBlocker>
#include <QStatusBar>
#include <QStyledItemDelegate>
#include <QTabBar>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>

#include <algorithm>
#include <cstdint>

namespace canvas::gui {

namespace {

// One Resolve-style project card (or the dashed "+ New Project" tile). Paints
// its own cover-fit thumbnail well, name + modified row, hover lift and the
// accent selection ring, so the grid view needs no icon plumbing.
class ProjectTileDelegate final : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;

    QSize sizeHint(const QStyleOptionViewItem&, const QModelIndex&) const override {
        return QSize(196, 176);
    }

    void paint(QPainter* p, const QStyleOptionViewItem& opt,
               const QModelIndex& index) const override {
        const ThemeTokens& t = tokens();
        const bool selected = opt.state & QStyle::State_Selected;
        const bool hovered = opt.state & QStyle::State_MouseOver;
        const bool is_new = index.data(kProjectNewRole).toBool();
        const bool missing = index.data(kProjectMissingRole).toBool();

        p->setRenderHint(QPainter::Antialiasing, true);

        const QRectF face(opt.rect.adjusted(1, 1, -1, -1));
        constexpr qreal kRadius = 10.0;

        if (hovered) {
            p->setPen(Qt::NoPen);
            p->setBrush(QColor(0, 0, 0, 50));
            p->drawRoundedRect(face.translated(0, 2), kRadius, kRadius);
        }

        if (is_new) {
            paint_new_tile(p, face, t);
        } else {
            paint_card(p, face, index, t, selected, hovered, missing);
        }

        if (selected) {
            p->setBrush(Qt::NoBrush);
            QPen ring(t.accent);
            ring.setWidthF(2.0);
            p->setPen(ring);
            p->drawRoundedRect(face.adjusted(1.0, 1.0, -1.0, -1.0), kRadius, kRadius);
        }
    }

private:
    static void paint_new_tile(QPainter* p, const QRectF& face, const ThemeTokens& t) {
        constexpr qreal kRadius = 10.0;
        p->setBrush(Qt::NoBrush);
        QPen dash(t.accent);
        dash.setStyle(Qt::DashLine);
        dash.setWidthF(1.5);
        dash.setDashPattern({4.0, 3.0});
        p->setPen(dash);
        p->drawRoundedRect(face, kRadius, kRadius);

        // Plus glyph, round-capped, with a centered gap like Resolve's tile.
        constexpr qreal kGlyph = 40.0;
        constexpr qreal kGap = 16.0;
        constexpr qreal kArm = (kGlyph - kGap) / 2.0;
        const QPointF c(face.center().x(), face.top() + face.height() * 0.38);
        QPen plus(t.accent);
        plus.setWidthF(2.0);
        plus.setCapStyle(Qt::RoundCap);
        p->setPen(plus);
        p->drawLine(QPointF(c.x() - kGlyph / 2.0, c.y()),
                    QPointF(c.x() - kGlyph / 2.0 + kArm, c.y()));
        p->drawLine(QPointF(c.x() + kGlyph / 2.0 - kArm, c.y()),
                    QPointF(c.x() + kGlyph / 2.0, c.y()));
        p->drawLine(QPointF(c.x(), c.y() - kGlyph / 2.0),
                    QPointF(c.x(), c.y() - kGlyph / 2.0 + kArm));
        p->drawLine(QPointF(c.x(), c.y() + kGlyph / 2.0 - kArm),
                    QPointF(c.x(), c.y() + kGlyph / 2.0));

        QFont f = p->font();
        f.setPixelSize(12);
        f.setWeight(QFont::Medium);
        const QFontMetricsF fm(f);
        const QString label = ProjectManagerWidget::tr("New Project");
        p->setFont(f);
        p->setPen(t.accent_text);
        p->drawText(QRectF(face.left() + 8, face.bottom() - fm.height() - 14,
                           face.width() - 16, fm.height()),
                    Qt::AlignCenter, fm.elidedText(label, Qt::ElideRight, face.width() - 16));
    }

    static void paint_card(QPainter* p, const QRectF& face, const QModelIndex& index,
                           const ThemeTokens& t, bool selected, bool hovered, bool missing) {
        // Raised card face; missing files collapse to the lower surface.
        QLinearGradient g(face.topLeft(), face.bottomLeft());
        if (missing) {
            g.setColorAt(0, t.surface_low);
            g.setColorAt(1, t.surface_low);
        } else {
            g.setColorAt(0, t.surface_higher);
            g.setColorAt(1, t.surface_raised);
        }
        p->setPen(Qt::NoPen);
        p->setBrush(g);
        p->drawRoundedRect(face, 10.0, 10.0);

        const QRectF inner = face.adjusted(10, 10, -10, -10);
        const qreal well_h = inner.width() * 9.0 / 16.0;
        const QRectF well(inner.topLeft(), QSizeF(inner.width(), well_h));

        QPainterPath well_path;
        well_path.addRoundedRect(well, 8.0, 8.0);
        p->setClipPath(well_path);

        QLinearGradient well_g(well.topLeft(), well.bottomLeft());
        well_g.setColorAt(0, QColor(0x33, 0x42, 0x4f));
        well_g.setColorAt(1, QColor(0x1c, 0x2a, 0x36));
        p->fillPath(well_path, well_g);

        const QImage img = index.data(kProjectThumbRole).value<QImage>();
        if (img.isNull()) {
            const QSize isz(40, 40);
            const QPixmap stub =
                icon("film-strip", QColor(0x8b, 0x9b, 0xaa, 100)).pixmap(isz);
            if (!stub.isNull()) {
                p->drawPixmap(QPointF(well.center().x() - isz.width() / 2.0,
                                      well.center().y() - isz.height() / 2.0),
                              stub);
            }
        } else {
            const qreal s = std::max(well.width() / static_cast<qreal>(img.width()),
                                     well.height() / static_cast<qreal>(img.height()));
            const QSizeF dst(img.width() * s, img.height() * s);
            const QRectF target(well.center().x() - dst.width() / 2.0,
                                well.center().y() - dst.height() / 2.0,
                                dst.width(), dst.height());
            p->drawImage(target, img);
        }
        p->setClipping(false);

        // Name + modified rows below the well.
        QFont name_f = p->font();
        name_f.setPixelSize(12);
        name_f.setWeight(QFont::Medium);
        const QFontMetricsF name_fm(name_f);
        const QString name = index.data(Qt::DisplayRole).toString();
        p->setFont(name_f);
        p->setPen(missing ? t.ink_faint : t.ink);
        p->drawText(QRectF(inner.left(), well.bottom() + 9, inner.width(), name_fm.height()),
                    Qt::AlignLeft | Qt::AlignVCenter,
                    name_fm.elidedText(name, Qt::ElideRight, inner.width()));

        QFont meta_f = p->font();
        meta_f.setPixelSize(10);
        const QFontMetricsF meta_fm(meta_f);
        QString meta = index.data(kProjectMetaRole).toDateTime().toString(QStringLiteral("d MMM yyyy"));
        if (missing) meta = ProjectManagerWidget::tr("File missing");
        p->setFont(meta_f);
        p->setPen(t.ink_faint);
        p->drawText(QRectF(inner.left(), well.bottom() + name_fm.height() + 6,
                           inner.width(), meta_fm.height()),
                    Qt::AlignLeft | Qt::AlignVCenter,
                    meta_fm.elidedText(meta, Qt::ElideRight, inner.width()));

        // Card border: soft by default, brighter on hover.
        p->setBrush(Qt::NoBrush);
        QPen edge(hovered && !selected ? t.border_hi : t.border_soft);
        edge.setWidthF(1.0);
        p->setPen(edge);
        p->drawRoundedRect(face, 10.0, 10.0);
    }
};

}  // namespace

// --- Project-manager integration ----------------------------------------------

ProjectManagerWidget::ProjectManagerWidget(QWidget* parent) : QWidget(parent) {
    setObjectName(QStringLiteral("projectManagerPage"));

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    // --- Top strip: Local tab + search / sort / user. ----------------------
    auto* strip = new QWidget(this);
    strip->setObjectName(QStringLiteral("pmStrip"));
    auto* strip_layout = new QHBoxLayout(strip);
    strip_layout->setContentsMargins(28, 2, 28, 0);
    strip_layout->setSpacing(0);

    auto* tabs = new QTabBar(strip);
    tabs->setObjectName(QStringLiteral("pmTabs"));
    tabs->setDrawBase(false);
    tabs->setExpanding(false);
    tabs->addTab(tr("Local"));
    strip_layout->addWidget(tabs);

    strip_layout->addStretch(1);

    search_ = new QLineEdit(strip);
    search_->setObjectName(QStringLiteral("pmSearch"));
    search_->setPlaceholderText(tr("Search projects…"));
    search_->setClearButtonEnabled(true);
    search_->setFixedWidth(260);
    search_->addAction(icon("search"), QLineEdit::LeadingPosition);
    strip_layout->addWidget(search_);
    strip_layout->addSpacing(12);

    sort_combo_ = new QComboBox(strip);
    sort_combo_->setObjectName(QStringLiteral("pmSort"));
    sort_combo_->addItem(tr("Date Modified"), 0);
    sort_combo_->addItem(tr("Name"), 1);
    strip_layout->addWidget(sort_combo_);
    strip_layout->addSpacing(16);

    auto* user_chip = new QLabel(QStringLiteral("@%1").arg(QDir::home().dirName()), strip);
    user_chip->setStyleSheet(QStringLiteral("color: %1; font-size: 11px;").arg(css(tokens().ink_muted)));
    strip_layout->addWidget(user_chip);

    root->addWidget(strip);

    // --- Body: heading row + grid. -------------------------------------------
    auto* content = new QVBoxLayout();
    content->setContentsMargins(28, 16, 28, 16);
    content->setSpacing(0);
    root->addLayout(content, 1);

    auto* heading = new QLabel(tr("Projects"), this);
    QFont heading_f = heading->font();
    heading_f.setPixelSize(16);
    heading_f.setWeight(QFont::Bold);
    heading->setFont(heading_f);
    content->addWidget(heading);
    content->addSpacing(4);

    empty_hint_ = new QLabel(this);
    empty_hint_->setObjectName(QStringLiteral("pmEmpty"));
    content->addWidget(empty_hint_);
    content->addSpacing(6);

    grid_ = new QListWidget(this);
    grid_->setObjectName(QStringLiteral("pmGrid"));
    grid_->setViewMode(QListView::IconMode);
    grid_->setGridSize(QSize(208, 192));
    grid_->setUniformItemSizes(true);
    grid_->setResizeMode(QListView::Adjust);
    grid_->setMovement(QListView::Static);
    grid_->setSelectionMode(QAbstractItemView::SingleSelection);
    grid_->setSpacing(6);
    grid_->setWordWrap(false);
    grid_->setMouseTracking(true);
    grid_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    grid_->setContextMenuPolicy(Qt::CustomContextMenu);
    grid_->setItemDelegate(new ProjectTileDelegate(grid_));
    content->addWidget(grid_, 1);

    // --- Footer: New / Import. -----------------------------------------------
    auto* footer = new QHBoxLayout();
    footer->setContentsMargins(28, 0, 28, 16);
    auto* new_btn = new QPushButton(tr("+  New Project"), this);
    new_btn->setObjectName(QStringLiteral("pmNew"));
    footer->addWidget(new_btn);
    footer->addSpacing(12);
    auto* import_btn = new QPushButton(tr("Import Project…"), this);
    import_btn->setObjectName(QStringLiteral("pmImport"));
    footer->addWidget(import_btn);
    footer->addStretch(1);
    root->addLayout(footer);

    // --- Styling (re-applied on theme switch via apply_theme_style). -------
    apply_theme_style(this, [this] {
        const ThemeTokens& t = tokens();
        return QStringLiteral(
            "QWidget#projectManagerPage { background: %1; }"
            "QWidget#pmStrip { background: %1; border-bottom: 1px solid %2; }"
            "QTabBar#pmTabs::tab { background: transparent; color: %3; padding: 9px 16px; "
            "border: none; border-bottom: 2px solid transparent; font-weight: 500; }"
            "QTabBar#pmTabs::tab:hover:!selected { color: %4; }"
            "QTabBar#pmTabs::tab:selected { color: %5; border-bottom-color: %6; font-weight: 600; }"
            "QLineEdit#pmSearch, QComboBox#pmSort { background: %7; border: 1px solid %8; "
            "border-radius: 8px; padding: 6px 10px; color: %3; }"
            "QLineEdit#pmSearch:focus, QComboBox#pmSort:focus { border: 1px solid %9; }"
            "QComboBox#pmSort::drop-down, QComboBox#pmSort::down-arrow { border: none; width: 0; }"
            "QListWidget#pmGrid { background: transparent; border: none; outline: none; }"
            "QLabel#pmEmpty { color: %10; font-size: 11px; }"
            "QPushButton { border-radius: 8px; padding: 8px 16px; font-size: 12px; }"
            "QPushButton#pmNew { background: %11; color: %12; font-weight: 600; }"
            "QPushButton#pmNew:hover { background: %13; }"
            "QPushButton#pmImport { background: transparent; border: 1px solid %9; color: %14; }"
            "QPushButton#pmImport:hover { background: %15; }")
            .arg(css(t.surface), css(t.border_soft), css(t.ink_muted), css(t.ink),
                 css(t.accent_text), css(t.accent), css(t.surface_raised), css(t.border),
                 css(t.accent_line), css(t.ink_faint), css(t.accent), css(t.on_accent),
                 css(t.accent_hover), css(t.accent_text), css(t.accent_soft));
    });

    // --- Wiring. --------------------------------------------------------------
    connect(search_, &QLineEdit::textChanged, this, [this](const QString& text) { filter_grid(text); });
    connect(sort_combo_, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            [this] { apply_sort(); });
    connect(grid_, &QListWidget::itemActivated, this,
            [this](QListWidgetItem* item) { activate_item(item); });
    connect(grid_, &QListWidget::customContextMenuRequested, this,
            [this](const QPoint& pos) { open_context_menu(grid_->viewport()->mapToGlobal(pos)); });
    connect(new_btn, &QPushButton::clicked, this, &ProjectManagerWidget::new_project_requested);
    connect(import_btn, &QPushButton::clicked, this, &ProjectManagerWidget::import_project_requested);

    new QShortcut(Qt::Key_Return, grid_, this, [this] { activate_current(); });
    new QShortcut(Qt::Key_Enter, grid_, this, [this] { activate_current(); });

    // Let the first refresh happen on the event loop: enter_project_manager has
    // already connected the thumbnail bridge, so a refresh here never drops a
    // card thumbnail before MainWindow's thumbnail_ready route exists.
    QTimer::singleShot(0, this, [this] { refresh(); });
}

void ProjectManagerWidget::refresh() {
    const QStringList recent = QSettings().value(QStringLiteral("recentProjects")).toStringList();
    QStringList unique;
    for (const QString& path : recent) {
        if (!unique.contains(path)) unique.append(path);
    }

    QSignalBlocker blocker(grid_);
    grid_->clear();
    cards_.clear();

    add_new_project_tile();
    int token = 1;
    for (const QString& path : unique)
        add_project_card(path, token++);

    apply_sort();
    filter_grid(search_->text());
}

void ProjectManagerWidget::add_new_project_tile() {
    auto* item = new QListWidgetItem();
    item->setText(tr("New Project"));
    item->setData(kProjectNewRole, true);
    grid_->addItem(item);
}

void ProjectManagerWidget::add_project_card(const QString& path, int token) {
    const QFileInfo fi(path);
    const bool missing = !fi.exists();

    auto* item = new QListWidgetItem();
    item->setText(fi.completeBaseName());
    item->setData(kProjectPathRole, path);
    if (!missing)
        item->setData(kProjectMetaRole, fi.lastModified());
    item->setData(kProjectMissingRole, missing);
    item->setData(kProjectTokenRole, token);
    item->setToolTip(path);
    grid_->addItem(item);
    cards_.push_back(item);

    if (!missing)
        emit thumbnail_requested(token, path);
}

void ProjectManagerWidget::apply_sort() {
    const bool by_name = sort_combo_->currentData().toInt() == 1;
    std::stable_sort(cards_.begin(), cards_.end(),
                     [by_name](QListWidgetItem* a, QListWidgetItem* b) {
                         if (by_name)
                             return a->text().compare(b->text(), Qt::CaseInsensitive) < 0;
                         return a->data(kProjectMetaRole).toDateTime() >
                                b->data(kProjectMetaRole).toDateTime();
                     });
    QSignalBlocker blocker(grid_);
    for (auto* it : cards_)
        grid_->takeItem(grid_->row(it));
    for (int i = 0; i < static_cast<int>(cards_.size()); ++i)
        grid_->insertItem(i + 1, cards_.at(i));
}

void ProjectManagerWidget::filter_grid(const QString& text) {
    const QString needle = text.trimmed();
    int visible = 0;
    for (int i = 0; i < grid_->count(); ++i) {
        QListWidgetItem* it = grid_->item(i);
        const bool is_new = it->data(kProjectNewRole).toBool();
        const bool match = is_new || needle.isEmpty() ||
                           it->text().contains(needle, Qt::CaseInsensitive);
        it->setHidden(!match);
        if (!is_new && match) ++visible;
    }
    if (cards_.empty()) {
        empty_hint_->setText(tr("No projects yet — create one to start editing."));
        empty_hint_->setVisible(true);
    } else if (visible == 0) {
        empty_hint_->setText(tr("No projects match your search."));
        empty_hint_->setVisible(true);
    } else {
        empty_hint_->setVisible(false);
    }
}

void ProjectManagerWidget::activate_item(QListWidgetItem* item) {
    if (!item) return;
    if (item->data(kProjectNewRole).toBool()) {
        emit new_project_requested();
        return;
    }
    emit open_project_requested(item->data(kProjectPathRole).toString());
}

void ProjectManagerWidget::activate_current() {
    if (grid_->hasFocus() || !search_->hasFocus())
        activate_item(grid_->currentItem());
}

void ProjectManagerWidget::open_context_menu(const QPoint& global_pos) {
    QListWidgetItem* item =
        grid_->itemAt(grid_->viewport()->mapFromGlobal(global_pos));
    if (!item || item->data(kProjectNewRole).toBool()) return;
    const QString path = item->data(kProjectPathRole).toString();
    const bool missing = item->data(kProjectMissingRole).toBool();

    QMenu menu(this);
    apply_rounded_menu(&menu);
    menu.addAction(tr("&Open"), this, [this, path] { emit open_project_requested(path); });
    QAction* reveal = menu.addAction(tr("Reveal in &File Manager"), this, [this, path] {
        QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(path).absolutePath()));
    });
    reveal->setEnabled(!missing);
    menu.addAction(tr("&Remove from Recents"), this, [this, path] {
        QStringList recent = QSettings().value(QStringLiteral("recentProjects")).toStringList();
        recent.removeAll(path);
        QSettings().setValue(QStringLiteral("recentProjects"), recent);
        refresh();
    });
    menu.exec(global_pos);
}

void ProjectManagerWidget::set_card_thumbnail(int token, const QImage& image) {
    for (auto* it : cards_) {
        if (it->data(kProjectTokenRole).toInt() == token) {
            it->setData(kProjectThumbRole, image);
            if (grid_->viewport()) grid_->viewport()->update();
            return;
        }
    }
}

// --- Project-manager window ----------------------------------------------------

ProjectManagerWindow::ProjectManagerWindow(QWidget* parent) : QDialog(parent) {
    setObjectName(QStringLiteral("projectManagerWindow"));
    setWindowTitle(tr("Nova Canvas Studio — Project Manager"));
    setWindowIcon(raw_icon("app_icon"));
    setMinimumSize(760, 520);
    resize(1160, 700);

    // Center over the primary screen on first creation.
    if (const QScreen* screen = QApplication::primaryScreen()) {
        const QRect area = screen->availableGeometry();
        move(area.center().x() - width() / 2, area.center().y() - height() / 2);
    }

    // The page fills the whole window; the WM provides the title-bar chrome.
    widget_ = new ProjectManagerWidget(this);
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->addWidget(widget_);

    apply_theme_style(this, [this] {
        return QStringLiteral("QWidget#projectManagerWindow { background: %1; }")
            .arg(css(tokens().surface));
    });
}

void ProjectManagerWindow::closeEvent(QCloseEvent* event) {
    // Closing the manager returns to the editor instead of quitting: hide and
    // hand over. Not delete-on-close — MainWindow keeps one for the session.
    event->ignore();
    emit window_closed();
}

void MainWindow::enter_project_manager() {
    // Lazily build the manager window once; parented to the editor so it dies
    // with the app and never leaks.
    if (!project_manager_window_) {
        project_manager_window_ = new ProjectManagerWindow(this);
        ProjectManagerWidget* pm = project_manager_window_->content();

        // New/Open land on the real project actions; the dialog acceptance
        // itself decides whether to leave the manager (cancel stays put).
        QObject::connect(pm, &ProjectManagerWidget::new_project_requested, this,
                         [this] { on_new_project(); });
        QObject::connect(pm, &ProjectManagerWidget::import_project_requested, this,
                         [this] { on_open_project(); });
        QObject::connect(pm, &ProjectManagerWidget::open_project_requested, this,
                         [this](const QString& path) {
                             leave_project_manager();
                             open_file(path);
                         });
        QObject::connect(project_manager_window_, &ProjectManagerWindow::window_closed,
                         this, [this] { leave_project_manager(); });

        // Card-thumbnail bridge: load the .ncs, request a frame from its first
        // video media under the project namespace (0xE… prefix). Pool ids keep
        // bit-63 only, so they never collide with this route; the pool's ready
        // filter below in MainWindow guards the reverse.
        QObject::connect(pm, &ProjectManagerWidget::thumbnail_requested, this,
                         [this](int token, const QString& project_path) {
                             canvas::core::Project project;
                             std::string error;
                             if (!canvas::core::load_project(project, project_path.toStdString(), &error)) {
                                 qWarning() << "[pmgr] thumb load FAILED" << project_path
                                            << QString::fromStdString(error);
                                 return;
                             }
                             for (const auto& shot : project.media) {
                                 if (shot.width > 0 && shot.height > 0 && shot.total_frames > 0) {
                                     const int64_t frame =
                                         std::max<int64_t>(0, shot.total_frames / 2);
                                     qWarning().nospace()
                                         << "[pmgr] thumb request token=" << token
                                         << " path='" << shot.path.c_str()
                                         << "' frame=" << frame;
                                     ThumbRequest req;
                                     req.id = kProjectThumbNs | static_cast<uint64_t>(token);
                                     req.path = shot.path;
                                     req.frame = frame;
                                     req.target_width = 320;
                                     req.max_height = 180;
                                     thumbnails_.request(req);
                                     return;
                                 }
                             }
                         });
        QObject::connect(&thumbnails_, &ThumbnailService::thumbnail_ready, this,
                         [this](uint64_t id, QImage image) {
                             if ((id & kProjectThumbNs) != kProjectThumbNs) return;
                             const int token = static_cast<int>(id & ~kProjectThumbNs);
                             if (project_manager_window_)
                                 project_manager_window_->content()->set_card_thumbnail(token, image);
                         });
    }

    if (project_screen_active_) {
        project_manager_window_->raise();
        project_manager_window_->activateWindow();
        project_manager_window_->refresh();
        return;
    }
    project_screen_active_ = true;
    qWarning() << "[page] enter project manager";
    controller_.pause();
    project_manager_window_->show();
    project_manager_window_->raise();
    project_manager_window_->activateWindow();
    project_manager_window_->refresh();
    if (status_)
        status_->showMessage(tr("Project Manager — open or create a project to begin editing."));
}

void MainWindow::leave_project_manager() {
    if (!project_screen_active_) return;
    project_screen_active_ = false;
    qWarning() << "[page] leave project manager";
    if (project_manager_window_) project_manager_window_->hide();
    show();
    raise();
    activateWindow();
}

}  // namespace canvas::gui