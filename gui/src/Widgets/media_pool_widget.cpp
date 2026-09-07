#include "Widgets/media_pool_widget.hpp"

#include <QAbstractItemModel>
#include <QHBoxLayout>
#include <QPainter>
#include <QUrl>

MediaPoolWidget::MediaPoolWidget(QWidget* parent) : QListWidget(parent) {
    setViewMode(QListView::IconMode);
    setIconSize(QSize(120, 68));
    setGridSize(QSize(132, 110));
    setUniformItemSizes(true);
    setResizeMode(QListView::Adjust);
    setMovement(QListView::Static);
    setSelectionMode(QAbstractItemView::ExtendedSelection);
    setDragEnabled(true);
    setDragDropMode(QAbstractItemView::DragOnly);
    setDefaultDropAction(Qt::CopyAction);
    setAcceptDrops(true);
    setWordWrap(true);
    setMouseTracking(true);

    setup_empty_state();

    if (QAbstractItemModel* m = model()) {
        connect(m, &QAbstractItemModel::rowsInserted, this, &MediaPoolWidget::update_empty_state);
        connect(m, &QAbstractItemModel::rowsRemoved, this, &MediaPoolWidget::update_empty_state);
        connect(m, &QAbstractItemModel::modelReset, this, &MediaPoolWidget::update_empty_state);
    }
    update_empty_state();
}

void MediaPoolWidget::setup_empty_state() {
    // Material 3 dark surface for the pool area.
    empty_state_ = new QWidget(this);
    empty_state_->setObjectName(QStringLiteral("mediaPoolEmpty"));
    empty_state_->setAttribute(Qt::WA_TransparentForMouseEvents, false);
    empty_state_->setAcceptDrops(true);
    empty_state_->raise();
    empty_state_->installEventFilter(this);

    auto* layout = new QVBoxLayout(empty_state_);
    layout->setContentsMargins(16, 24, 16, 24);
    layout->setSpacing(14);

    // Primary label.
    auto* title = new QLabel(tr("No clips in media pool"), empty_state_);
    title->setAlignment(Qt::AlignCenter);
    title->setStyleSheet(
        "QLabel { color: #FFFFFF; font-size: 17px; font-weight: 500; background: transparent; }");

    // Secondary label.
    auto* subtitle = new QLabel(tr("Add clips from Media Storage to get started"), empty_state_);
    subtitle->setAlignment(Qt::AlignCenter);
    subtitle->setStyleSheet(
        "QLabel { color: #9E9E9E; font-size: 13px; font-weight: 400; background: transparent; }");

    // Blue accent CTA (Material 3 filled button, flat / no bevel).
    import_button_ = new QPushButton(tr("Import Media"), empty_state_);
    import_button_->setObjectName(QStringLiteral("mediaPoolAddButton"));
    import_button_->setCursor(Qt::PointingHandCursor);
    import_button_->setStyleSheet(
        "QPushButton#mediaPoolAddButton {"
        "  background-color: #3B82F6; color: #FFFFFF; border: none; border-radius: 8px;"
        "  padding: 8px 14px; font-size: 13px; font-weight: 500;"
        "}"
        "QPushButton#mediaPoolAddButton:hover { background-color: #4C92FF; }"
        "QPushButton#mediaPoolAddButton:pressed { background-color: #2F6FED; }"
        "QPushButton#mediaPoolAddButton:focus { outline: none; }");

    layout->addStretch();
    layout->addWidget(title);
    layout->addWidget(subtitle);
    layout->addSpacing(6);
    auto* btn_row = new QHBoxLayout;
    btn_row->addStretch();
    btn_row->addWidget(import_button_);
    btn_row->addStretch();
    layout->addLayout(btn_row);
    layout->addStretch();

    connect(import_button_, &QPushButton::clicked, this, &MediaPoolWidget::importRequested);
}

void MediaPoolWidget::update_empty_state() {
    if (!empty_state_) return;
    const bool empty = count() == 0;
    if (empty) {
        // Fill the visible viewport area (inside the frame / scroll margins).
        empty_state_->setGeometry(viewport()->geometry());
        empty_state_->raise();
    }
    empty_state_->setVisible(empty);
    viewport()->update();
}

void MediaPoolWidget::resizeEvent(QResizeEvent* event) {
    QListWidget::resizeEvent(event);
    update_empty_state();
}

bool MediaPoolWidget::event(QEvent* event) {
    // The Trim menu maps the bare Del key to "Ripple Delete" as a window-level
    // shortcut. A window shortcut fires before the focused widget ever sees the
    // key, so take the shortcut override when pool items are selected and let
    // the Del key reach keyPressEvent with its pool+clip meaning instead.
    if (event->type() == QEvent::ShortcutOverride) {
        auto* ke = static_cast<QKeyEvent*>(event);
        if (ke->key() == Qt::Key_Delete && !(ke->modifiers() & Qt::ShiftModifier) &&
            !selectedItems().isEmpty()) {
            ke->accept();
            return true;
        }
    }
    return QListWidget::event(event);
}

void MediaPoolWidget::keyPressEvent(QKeyEvent* event) {
    if (!selectedItems().isEmpty()) {
        if (event->key() == Qt::Key_Delete) {
            // In the pool the Del key is dual-purpose: drop the selected media
            // items AND ripple-delete any clip selected on the timeline.
            emit deleteSelectedWithClipsRequested();
            event->accept();
            return;
        }
        if (event->key() == Qt::Key_Backspace) {
            emit deleteSelectedRequested();
            event->accept();
            return;
        }
    }
    QListWidget::keyPressEvent(event);
}

void MediaPoolWidget::startDrag(Qt::DropActions supported) {
    QListWidgetItem* item = currentItem();
    if (!item) return;
    const QVariant v = item->data(Qt::UserRole);
    if (!v.isValid()) return;
    auto* md = new QMimeData;
    md->setData("application/x-eh-media-id", QByteArray::number(v.toLongLong()));
    auto* drag = new QDrag(this);
    drag->setMimeData(md);
    if (!item->icon().isNull()) drag->setPixmap(item->icon().pixmap(96, 54));
    drag->exec(Qt::CopyAction, Qt::CopyAction);
}

void MediaPoolWidget::dragEnterEvent(QDragEnterEvent* event) {
    if (event->mimeData()->hasUrls()) {
        event->acceptProposedAction();
        return;
    }
    QListWidget::dragEnterEvent(event);
}

void MediaPoolWidget::dragMoveEvent(QDragMoveEvent* event) {
    if (event->mimeData()->hasUrls()) {
        event->acceptProposedAction();
        return;
    }
    QListWidget::dragMoveEvent(event);
}

void MediaPoolWidget::dropEvent(QDropEvent* event) {
    if (!event->mimeData()->hasUrls()) {
        QListWidget::dropEvent(event);
        return;
    }
    QStringList paths;
    const auto urls = event->mimeData()->urls();
    for (const QUrl& url : urls) {
        if (url.isLocalFile()) paths.append(url.toLocalFile());
    }
    if (!paths.isEmpty()) emit filesDropped(paths);
    event->acceptProposedAction();
}

bool MediaPoolWidget::eventFilter(QObject* watched, QEvent* event) {
    // The empty-state overlay covers the viewport when the pool is empty, which
    // would otherwise swallow drag & drop. Forward file drops through it so
    // users can drag media in even before anything is imported.
    if (watched == empty_state_) {
        if (event->type() == QEvent::DragEnter) {
            QDragEnterEvent* de = static_cast<QDragEnterEvent*>(event);
            if (de->mimeData()->hasUrls()) {
                de->acceptProposedAction();
                return true;
            }
        } else if (event->type() == QEvent::DragMove) {
            QDragMoveEvent* dm = static_cast<QDragMoveEvent*>(event);
            if (dm->mimeData()->hasUrls()) {
                dm->acceptProposedAction();
                return true;
            }
        } else if (event->type() == QEvent::Drop) {
            QDropEvent* dp = static_cast<QDropEvent*>(event);
            if (dp->mimeData()->hasUrls()) {
                QStringList paths;
                const auto urls = dp->mimeData()->urls();
                for (const QUrl& url : urls) {
                    if (url.isLocalFile()) paths.append(url.toLocalFile());
                }
                if (!paths.isEmpty()) emit filesDropped(paths);
                dp->acceptProposedAction();
                return true;
            }
        }
    }
    return QListWidget::eventFilter(watched, event);
}
