#include "Widgets/media_pool_widget.hpp"

#include <QAbstractItemModel>
#include <QHBoxLayout>
#include <QPainter>
#include <QPainterPath>
#include <QStyledItemDelegate>
#include <QStyle>
#include <QUrl>

#include <cmath>

#include "UX/theme.hpp"

using canvas::gui::apply_theme_style;
using canvas::gui::css;
using canvas::gui::icon;
using canvas::gui::ThemeTokens;
using canvas::gui::tokens;
using canvas::gui::with_alpha;

namespace {

// Paints each pool entry as a tile card matching the Alt-html reference:
// a compact raised card (r-md), a 16:9 thumbnail well (tinted placeholder with
// a centered icon until the decodable frame/waveform lands, or a cover-fit
// thumbnail once loaded), a mono duration badge top-left, a media-type chip
// top-right, and a caption row with name + resolution/fps in mono. Hover
// brightens the border and lifts the card; selection adds the mint focus ring.
class MediaPoolTileDelegate final : public QStyledItemDelegate {
public:
    explicit MediaPoolTileDelegate(QObject* parent = nullptr) : QStyledItemDelegate(parent) {}

    QSize sizeHint(const QStyleOptionViewItem&, const QModelIndex&) const override {
        return QSize(120, 108);
    }

    void paint(QPainter* p, const QStyleOptionViewItem& opt,
               const QModelIndex& index) const override {
        const ThemeTokens& t = tokens();

        const bool selected = opt.state & QStyle::State_Selected;
        const bool hovered = opt.state & QStyle::State_MouseOver;
        const bool is_video = index.data(kPoolIsVideoRole).toBool();

        const qreal radius = 8;
        const QRectF cardRect(opt.rect.adjusted(2, 2, -2, -2));

        p->setRenderHint(QPainter::Antialiasing);

        // Hover lifts the tile onto a soft shadow-card.
        if (hovered) {
            QPainterPath halo;
            halo.addRoundedRect(cardRect.translated(0, 2), radius, radius);
            p->fillPath(halo, with_alpha(QColor(0, 0, 0), 60));
        }

        // Card face: reference tile bg is a flat surface-2; light-mode glass
        // keeps it subtle. A barely-lit top edge reads as elevation, not chrome.
        QLinearGradient face(cardRect.topLeft(), cardRect.bottomLeft());
        face.setColorAt(0.0, t.surface_higher);
        face.setColorAt(1.0, t.surface_raised);
        QPainterPath card;
        card.addRoundedRect(cardRect, radius, radius);
        p->fillPath(card, face);

        // 16:9 thumbnail well above the caption strip.
        const qreal thumbH = cardRect.width() * 9.0 / 16.0;
        QRectF thumbRect(cardRect.left(), cardRect.top(), cardRect.width(), thumbH);
        QPainterPath thumbClip;
        thumbClip.addRoundedRect(thumbRect, radius, radius);

        QPixmap pm;
        const QIcon ic = index.data(Qt::DecorationRole).value<QIcon>();
        if (!ic.isNull()) pm = ic.pixmap(thumbRect.size().toSize());

        if (pm.isNull()) {
            // Tinted placeholder well: slate for video, violet for audio, with a
            // centered micro-icon (opacity .7) like the reference .th svg.
            const QColor top = is_video ? QColor(0x33, 0x42, 0x4f) : QColor(0x2b, 0x21, 0x40);
            const QColor bot = is_video ? QColor(0x1c, 0x2a, 0x36) : QColor(0x1c, 0x16, 0x30);
            QLinearGradient well(thumbRect.topLeft(), thumbRect.bottomLeft());
            well.setColorAt(0.0, top);
            well.setColorAt(1.0, bot);
            p->fillPath(thumbClip, well);
            const QColor tint = is_video ? QColor(0x5f, 0x7a, 0x8a) : QColor(0x9d, 0x86, 0xd8);
            const QPixmap ph = icon(is_video ? "film-strip" : "volume", tint)
                                   .pixmap(QSize(18, 18));
            p->drawPixmap(thumbRect.center() - QPointF(ph.width(), ph.height()) / 2.0, ph);
        } else {
            // Scale-to-fill (cover) the well, centered, radius-clipped.
            const QSizeF src(pm.size());
            const qreal scale = qMax(thumbRect.width() / src.width(),
                                     thumbRect.height() / src.height());
            const QSizeF dst(src.width() * scale, src.height() * scale);
            const QRectF target(thumbRect.center() - QPointF(dst.width(), dst.height()) / 2.0, dst);
            p->save();
            p->setClipPath(thumbClip);
            p->drawPixmap(target.toRect(), pm);
            p->restore();
        }

        // Mono badges: duration top-left, media-type chip top-right, both on a
        // dark translucent pill like the reference.
        draw_badge(p, thumbRect.topLeft() + QPointF(6, 6),
                   index.data(kPoolDurationRole).toString());
        draw_type_chip(p, thumbRect, is_video);

        // Caption: name (10.5px fg) + mono resolution·fps line (9.5px meta).
        const QString name = index.data(Qt::DisplayRole).toString();
        const QString res = index.data(kPoolResolutionRole).toString();
        const QRectF cap(cardRect.left() + 7, thumbRect.bottom() + 3,
                         cardRect.width() - 14, cardRect.height() - thumbH - 5);

        QFont nf = opt.font;
        nf.setPointSizeF(10.5);
        nf.setWeight(QFont::Medium);
        p->setFont(nf);
        const QFontMetricsF nfm(nf);
        p->setPen(selected ? t.accent_text : t.ink);
        p->drawText(cap, Qt::AlignLeft | Qt::AlignTop,
                    nfm.elidedText(name, Qt::ElideRight, static_cast<int>(cap.width())));
        if (!res.isEmpty()) {
            QFont rf = nf;
            rf.setFamily(t.font_mono);
            rf.setPointSizeF(9.0);
            p->setFont(rf);
            p->setPen(with_alpha(t.ink_muted, 230));
            p->drawText(QRectF(cap.left(), cap.top() + nfm.height() + 1,
                               cap.width(), 14),
                        Qt::AlignLeft | Qt::AlignVCenter, res);
        }

        // Border: transparent idle, border_hi on hover, mint ring when selected.
        QPen border(selected ? t.accent : (hovered ? t.border_hi : QColor(0, 0, 0, 0)), 1);
        p->setPen(border);
        p->setBrush(Qt::NoBrush);
        p->drawPath(card);
        if (selected) {
            p->setPen(with_alpha(t.accent, 90));
            p->setBrush(Qt::NoBrush);
            p->drawRoundedRect(cardRect.adjusted(-3, -3, 3, 3), radius + 1, radius + 1);
        }
    }

private:
    static void draw_badge(QPainter* p, const QPointF& topLeft, const QString& text) {
        if (text.isEmpty()) return;
        QFont f;
        f.setFamily(tokens().font_mono);
        f.setPointSizeF(9.0);
        f.setWeight(QFont::Medium);
        p->setFont(f);
        const QFontMetricsF fm(f);
        const qreal w = fm.horizontalAdvance(text) + 9;
        const QRectF pill(topLeft, QSizeF(w, fm.height() + 2));
        p->fillRect(pill, QColor(4, 8, 14, 200));
        p->setPen(QColor(0xDF, 0xE5, 0xEE));
        p->drawText(pill, Qt::AlignCenter, text);
    }

    static void draw_type_chip(QPainter* p, const QRectF& thumb, bool is_video) {
        const QRectF chip(thumb.right() - 21, thumb.top() + 6, 16, 16);
        p->fillRect(chip, QColor(4, 8, 14, 200));
        p->setPen(QColor(0xDF, 0xE5, 0xEE));
        p->setBrush(Qt::NoBrush);
        const QPointF c = chip.center();
        if (is_video) {
            // Tiny film frame (rounded rect).
            const QRectF frame(c.x() - 6, c.y() - 4.5, 12, 9);
            p->drawRoundedRect(frame, 1.5, 1.5);
        } else {
            // Audio waveform bars.
            for (int i = -3; i <= 3; i += 2) {
                const qreal h = (i == 0) ? 8.0 : (std::abs(i) <= 1 ? 6.0 : 3.5);
                const qreal x = c.x() + i * 1.8;
                p->drawLine(QPointF(x, c.y() - h / 2.0), QPointF(x, c.y() + h / 2.0));
            }
        }
    }
};

}  // namespace

MediaPoolWidget::MediaPoolWidget(QWidget* parent) : QListWidget(parent) {
    setViewMode(QListView::IconMode);
    setIconSize(QSize(0, 0));
    setGridSize(QSize(124, 110));
    setUniformItemSizes(true);
    setResizeMode(QListView::Adjust);
    setMovement(QListView::Static);
    setSelectionMode(QAbstractItemView::ExtendedSelection);
    setDragEnabled(true);
    setDragDropMode(QAbstractItemView::DragOnly);
    setDefaultDropAction(Qt::CopyAction);
    setAcceptDrops(true);
    setWordWrap(false);
    setMouseTracking(true);
    setItemDelegate(new MediaPoolTileDelegate(this));
    setSpacing(6);

    setup_empty_state();

    if (QAbstractItemModel* m = model()) {
        connect(m, &QAbstractItemModel::rowsInserted, this, &MediaPoolWidget::update_empty_state);
        connect(m, &QAbstractItemModel::rowsRemoved, this, &MediaPoolWidget::update_empty_state);
        connect(m, &QAbstractItemModel::modelReset, this, &MediaPoolWidget::update_empty_state);
    }
    update_empty_state();
}

void MediaPoolWidget::setup_empty_state() {
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
    apply_theme_style(title, [] {
        return QStringLiteral(
            "QLabel { color: %1; font-size: 17px; font-weight: 500; background: transparent; }")
            .arg(css(tokens().ink));
    });

    // Secondary label.
    auto* subtitle = new QLabel(tr("Add clips from Media Storage to get started"), empty_state_);
    subtitle->setAlignment(Qt::AlignCenter);
    apply_theme_style(subtitle, [] {
        return QStringLiteral(
            "QLabel { color: %1; font-size: 13px; font-weight: 400; background: transparent; }")
            .arg(css(tokens().ink_faint));
    });

    // Accent CTA (filled button, flat / no bevel).
    import_button_ = new QPushButton(tr("Import Media"), empty_state_);
    import_button_->setObjectName(QStringLiteral("mediaPoolAddButton"));
    import_button_->setCursor(Qt::PointingHandCursor);
    apply_theme_style(import_button_, [] {
        const ThemeTokens& t = tokens();
        return QStringLiteral(
            "QPushButton#mediaPoolAddButton {"
            "  background-color: %1; color: %2; border: none; border-radius: 8px;"
            "  padding: 8px 14px; font-size: 13px; font-weight: 500;"
            "}"
            "QPushButton#mediaPoolAddButton:hover { background-color: %3; }"
            "QPushButton#mediaPoolAddButton:pressed { background-color: %4; }"
            "QPushButton#mediaPoolAddButton:focus { outline: none; }")
            .arg(css(t.accent), css(t.on_accent), css(t.accent_hover),
                 css(t.accent_press));
    });

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
