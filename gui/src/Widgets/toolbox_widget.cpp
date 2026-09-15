#include "Widgets/toolbox_widget.hpp"

#include "UX/empty_state.hpp"
#include "UX/theme.hpp"

#include <QApplication>
#include <QDrag>
#include <QFont>
#include <QFontMetricsF>
#include <QFrame>
#include <QListWidgetItem>
#include <QMimeData>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
#include <QStyle>
#include <QStyledItemDelegate>
#include <QStyleOptionViewItem>
#include <QTabWidget>
#include <QVBoxLayout>

#include <cstring>

namespace canvas::gui {

namespace {

// Item-data roles past the drag mime/payload (UserRole, UserRole + 1).
constexpr int kToolboxMetaRole = Qt::UserRole + 2;
constexpr int kToolboxKindRole = Qt::UserRole + 3;
constexpr int kToolboxIconRole = Qt::UserRole + 4;

// Tile card geometry. Cell size + between-cell spacing; Qt's IconMode grid
// layout spreads the columns evenly across the full well width like the Media
// Pool grid above.
constexpr int kGridW = 112;
constexpr int kGridH = 98;
constexpr int kGridSpacing = 8;

// Paints each catalogue entry as a tile card in the Media Pool's own idiom: a
// compact raised card (r-8) with a flat face, a flat tinted well carrying the
// centred kind icon, and a caption row (name + mono meta). Flat fills only —
// gradients band on 8-bit panels. Hover lifts the tile and lights the border;
// selection adds the amber focus ring + outer glow. Effect tiles ("SOON")
// paint dimmed and skip the lift — catalogue only, never a drag source.
class ToolboxTileDelegate final : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;

    QSize sizeHint(const QStyleOptionViewItem&, const QModelIndex&) const override {
        return QSize(108, 94);
    }

    void paint(QPainter* p, const QStyleOptionViewItem& opt,
               const QModelIndex& index) const override {
        const ThemeTokens& t = tokens();

        const bool selected = opt.state & QStyle::State_Selected;
        const bool hovered = opt.state & QStyle::State_MouseOver;
        const auto kind =
            static_cast<ToolboxKind>(index.data(kToolboxKindRole).toInt());
        const bool soon = (kind == ToolboxKind::Effect);
        const QString meta = index.data(kToolboxMetaRole).toString();

        constexpr qreal kRadius = 8.0;
        constexpr qreal kWellH = 52.0;
        const QRectF card(opt.rect.adjusted(2, 2, -2, -2));

        p->setRenderHint(QPainter::Antialiasing);

        // Hover lifts the tile onto a soft shadow-card (draggable kinds only).
        if (hovered && !soon) {
            QPainterPath halo;
            halo.addRoundedRect(card.translated(0, 2), kRadius, kRadius);
            p->fillPath(halo, with_alpha(QColor(0, 0, 0), 60));
        }

        // Card face: flat surface fill — gradients band on 8-bit panels.
        QPainterPath cardPath;
        cardPath.addRoundedRect(card, kRadius, kRadius);
        p->fillPath(cardPath, t.surface_raised);

        // Flat tinted well + centred kind icon. Slate for titles (footage-like
        // payload, same placeholder the pool paints for video), deep teal for
        // transitions, violet for the not-yet-shipped effects.
        QColor wellFill, iconTint;
        switch (kind) {
            case ToolboxKind::Title:
                wellFill = QColor(0x1c, 0x2a, 0x36);
                iconTint = QColor(0x5f, 0x7a, 0x8a);
                break;
            case ToolboxKind::Transition:
                wellFill = QColor(0x12, 0x25, 0x28);
                iconTint = QColor(0x6f, 0xb9, 0xbd);
                break;
            case ToolboxKind::Effect:
                wellFill = QColor(0x1c, 0x16, 0x30);
                iconTint = QColor(0x9d, 0x86, 0xd8);
                break;
        }
        const QRectF well(card.left(), card.top(), card.width(), kWellH);
        QPainterPath wellClip;
        wellClip.addRoundedRect(well, kRadius, kRadius);
        p->save();
        if (soon) p->setOpacity(0.55);
        p->fillPath(wellClip, wellFill);
        // The well icon is re-tinted per kind from the bundled SVG name (the
        // pool's own placeholder idiom) — QIcon carries no usable name here.
        const QByteArray icon_name = index.data(kToolboxIconRole).toByteArray();
        if (!icon_name.isEmpty()) {
            const QPixmap ph = icon(icon_name.constData(), iconTint).pixmap(QSize(20, 20));
            if (!ph.isNull())
                p->drawPixmap(well.center() - QPointF(ph.width(), ph.height()) / 2.0, ph);
        }
        p->restore();

        // Caption: name (10.5px medium) + mono meta line (9px), like the pool.
        const QString name = index.data(Qt::DisplayRole).toString();
        const QRectF cap(card.left() + 7, well.bottom() + 3, card.width() - 14,
                         card.height() - kWellH - 5);
        QFont nf = opt.font;
        nf.setPointSizeF(10.5);
        nf.setWeight(QFont::Medium);
        p->setFont(nf);
        const QFontMetricsF nfm(nf);
        p->setPen(selected ? t.accent_text : (soon ? t.ink_faint : t.ink));
        p->drawText(cap, Qt::AlignHCenter | Qt::AlignTop,
                    nfm.elidedText(name, Qt::ElideRight, static_cast<int>(cap.width())));
        if (!meta.isEmpty()) {
            QFont rf = nf;
            rf.setFamily(t.font_mono);
            rf.setPointSizeF(9.0);
            p->setFont(rf);
            p->setPen(soon ? t.ink_faint : with_alpha(t.ink_muted, 230));
            const QFontMetricsF rfm(rf);
            p->drawText(QRectF(cap.left(), cap.top() + nfm.height() + 1, cap.width(), 14),
                        Qt::AlignHCenter | Qt::AlignVCenter,
                        rfm.elidedText(meta, Qt::ElideRight, static_cast<int>(cap.width())));
        }

        // Border: transparent idle, border_hi on hover, amber ring + outer
        // glow when selected — the pool's focus language verbatim.
        QPen border(selected ? t.accent : (hovered ? t.border_hi : QColor(0, 0, 0, 0)), 1);
        p->setPen(border);
        p->setBrush(Qt::NoBrush);
        p->drawPath(cardPath);
        if (selected) {
            p->setPen(with_alpha(t.accent, 90));
            p->setBrush(Qt::NoBrush);
            p->drawRoundedRect(card.adjusted(-3, -3, 3, 3), kRadius + 1, kRadius + 1);
        }
    }
};

}  // namespace

// --- ToolboxList ------------------------------------------------------------
ToolboxList::ToolboxList(QWidget* parent) : QListWidget(parent) {
    // Grid geometry mirrors the Media Pool (IconMode tile cards, static flow).
    // The well background is the pool's own style function — same surface_low
    // fill + 1px border, tracking theme switches identically. The delegate
    // paints every tile including hover/selection. Mouse tracking is what lets
    // the delegate's hover state fire at all.
    setViewMode(QListView::IconMode);
    setIconSize(QSize(0, 0));
    setGridSize(QSize(kGridW, kGridH));
    setUniformItemSizes(true);
    setResizeMode(QListView::Adjust);
    setMovement(QListView::Static);
    setSelectionMode(QAbstractItemView::SingleSelection);
    setDragEnabled(true);
    setDragDropMode(QAbstractItemView::DragOnly);
    setDefaultDropAction(Qt::CopyAction);
    setWordWrap(false);
    setMouseTracking(true);
    setItemDelegate(new ToolboxTileDelegate(this));
    setSpacing(kGridSpacing);
    setFrameShape(QFrame::NoFrame);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    setEditTriggers(QAbstractItemView::NoEditTriggers);
    apply_theme_style(this, &media_pool_style);
}

QListWidgetItem* ToolboxList::add_item(const QString& text, ToolboxKind kind,
                                       const QString& meta, const char* mime,
                                       const QByteArray& payload, const char* icon_name) {
    auto* item = new QListWidgetItem(text, this);
    item->setFlags(Qt::ItemIsSelectable | Qt::ItemIsEnabled | Qt::ItemIsDragEnabled);
    if (mime && mime[0] != '\0' && !payload.isEmpty()) {
        item->setData(Qt::UserRole, QString::fromLatin1(mime));
        item->setData(Qt::UserRole + 1, payload);
    } else {
        item->setFlags(item->flags() & ~Qt::ItemIsDragEnabled);
    }
    item->setData(kToolboxMetaRole, meta);
    item->setData(kToolboxKindRole, static_cast<int>(kind));
    if (icon_name && icon_name[0] != '\0')
        item->setData(kToolboxIconRole, QByteArray(icon_name));
    // The drop behaviour lives in the hover tooltip (the meta line shows a
    // content preview instead): titles land on a new video track, transitions
    // apply to the clip under the drop point.
    switch (kind) {
        case ToolboxKind::Title:
            item->setToolTip(
                tr("Drag onto the timeline — places “%1” on a new video track").arg(text));
            break;
        case ToolboxKind::Transition:
            item->setToolTip(tr("Drag onto a clip — applies to the clip's edit edge"));
            break;
        case ToolboxKind::Effect:
            item->setToolTip(tr("Coming in a future update"));
            break;
    }
    addItem(item);
    return item;
}

void ToolboxList::startDrag(Qt::DropActions supported) {    QListWidgetItem* item = currentItem();
    if (!item) return;
    const QVariant mime = item->data(Qt::UserRole);
    if (!mime.isValid()) return;
    const QByteArray payload = item->data(Qt::UserRole + 1).toByteArray();

    auto* md = new QMimeData;
    md->setData(mime.toString().toLatin1(), payload);
    auto* drag = new QDrag(this);
    drag->setMimeData(md);

// Mini card tile: the catalogue card shrunk to a drag chip (flat face,
// border, well icon + DemiBold label) so the timeline reads the incoming
// payload as "clip being placed".
    const ThemeTokens& t = tokens();
    QPixmap tile(160, 64);
    tile.fill(Qt::transparent);
    {
        QPainter p(&tile);
        p.setRenderHint(QPainter::Antialiasing);
        p.setPen(QPen(t.border, 1));
        p.setBrush(t.surface_raised);
        p.drawRoundedRect(QRectF(1.5, 1.5, 157, 61), 8, 8);
        const QByteArray icon_name = item->data(kToolboxIconRole).toByteArray();
        if (!icon_name.isEmpty()) {
            const QPixmap ph = icon(icon_name.constData(), t.ink).pixmap(QSize(20, 20));
            if (!ph.isNull()) p.drawPixmap(QPointF(14, 22), ph);
        }
        QFont f = QApplication::font();
        f.setPointSizeF(11.0);
        f.setWeight(QFont::DemiBold);
        p.setFont(f);
        p.setPen(t.ink);
        p.drawText(QRectF(44, 8, 106, 48),
                   Qt::AlignLeft | Qt::AlignVCenter | Qt::TextSingleLine, item->text());
    }
    drag->setPixmap(tile);

    drag->exec(supported ? supported : Qt::CopyAction, Qt::CopyAction);
    delete drag;
}

// --- ToolboxWidget ----------------------------------------------------------
const std::vector<ToolboxWidget::TitlePreset>& ToolboxWidget::title_presets() {
    static const std::vector<TitlePreset> kPresets{
        {"text",       "Text",          "Text",               0.10f},
        {"title",      "Title",         "Title",              0.16f},
        {"lowerthird", "Lower Third",   "LOWER THIRD",        0.08f},
        {"subtitle",   "Subtitle",      "Subtitle",           0.07f},
        {"bigtitle",   "Big Title",     "BIG TITLE",          0.24f},
        {"credit",     "Credit Roll",   "Credit Roll",        0.09f},
    };
    return kPresets;
}

ToolboxWidget::ToolboxWidget(QWidget* parent) : QWidget(parent) {
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(8, 6, 8, 8);
    root->setSpacing(6);

    auto* caps = new QLabel(tr("Toolbox"), this);
    caps->setText(caps->text().toUpper());
    apply_theme_style(caps, [] {
        return QStringLiteral(
            "color: %1; font-size: 10px; font-weight: 600; letter-spacing: 0.08em;"
            " text-transform: uppercase; padding: 0 4px; background-color: transparent;")
            .arg(css(tokens().ink_muted));
    });
    root->addWidget(caps);

    tabs_ = new QTabWidget(this);
    tabs_->setObjectName(QStringLiteral("leftTabStrip"));
    tabs_->setTabPosition(QTabWidget::North);
    tabs_->setDocumentMode(true);
    apply_theme_style(tabs_, &left_tab_strip_style);
    tabs_->addTab(build_effects_tab(), tr("Effects"));
    tabs_->addTab(build_titles_tab(), tr("Titles"));
    tabs_->addTab(build_transitions_tab(), tr("Transitions"));
    tabs_->addTab(build_more_tab(), tr("More"));
    root->addWidget(tabs_, 1);
}

QWidget* ToolboxWidget::build_effects_tab() {
    effects_ = new ToolboxList(this);
    for (const char* name : {"Blur", "Sharpen", "Film Grain", "Glow", "Vignette", "Colour Boost"})
        effects_->add_item(tr(name), ToolboxKind::Effect, tr("SOON"), nullptr, QByteArray(),
                           "effects");
    return effects_;
}

QWidget* ToolboxWidget::build_titles_tab() {
    titles_ = new ToolboxList(this);
    for (const TitlePreset& p : title_presets())
        // Meta line previews the preset's default on-screen text (content, so
        // never translated) rather than labelling the drop behaviour.
        titles_->add_item(tr(p.label), ToolboxKind::Title, QString::fromUtf8(p.sample),
                          "application/x-eh-title", QByteArray(p.id), "text");
    return titles_;
}

QWidget* ToolboxWidget::build_transitions_tab() {
    transitions_ = new ToolboxList(this);
    const struct { const char* id; const char* label; } rows[] = {
        {"cross",     "Cross Dissolve"},
        {"dipblack",  "Dip To Black"},
        {"fadein",    "Video Fade In"},
        {"fadeout",   "Video Fade Out"},
        {"wipelt",    "Wipe Left"},
        {"wipert",    "Wipe Right"},
        {"wipeup",    "Wipe Up"},
        {"wipedn",    "Wipe Down"},
    };
    for (const auto& r : rows) {
        const bool fades_in = std::strcmp(r.id, "fadein") == 0;
        transitions_->add_item(tr(r.label), ToolboxKind::Transition,
                               fades_in ? tr("IN EDGE") : tr("OUT EDGE"),
                               "application/x-eh-transition", QByteArray(r.id),
                               "transition");
    }
    return transitions_;
}

QWidget* ToolboxWidget::build_more_tab() {
    return build_empty_state(this, "mode", tr("More in a future update"),
                             tr("Sound library, automations, keyframes and the full "
                                "effect catalogue land in later releases."));
}

}  // namespace canvas::gui
