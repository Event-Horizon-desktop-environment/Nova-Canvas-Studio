#pragma once

// The Toolbox — the Resolve-style library panel that fills the lower half of
// the left dock, directly beneath the Media Pool. A compact tab strip
// (Effects | Titles | Transitions | More) whose tabs hold tile-card catalogues
// in the Media Pool's own idiom (rounded flat card — no gradients, they band
// on 8-bit panels — flat tinted well with a centered icon, name + mono meta
// caption, amber selection ring, hover lift).
// Tiles are drag sources:
//   - Titles drag with the `application/x-eh-title` mime (payload = stable
//     preset id); a drop on the timeline places a media-less title clip on a
//     BRAND-NEW top video track (never clobbering footage).
//   - Transitions drag with the `application/x-eh-transition` mime (payload =
//     stable TransitionType id); a drop over a clip body applies that
//     transition to the clip's edit.
// Effects tiles are dimmed catalogue rows for now (meta "SOON", no payload).
//
// QT-LINKED (deliberately): this is pure chrome; the placement law itself
// lives in MainWindow::place_title_at (ProjectActions.cpp).

#include <QByteArray>
#include <QIcon>
#include <QListWidget>
#include <QWidget>

#include <vector>

class QTabWidget;

namespace canvas::gui {

// Catalogue kind: drives the tile's well tint + icon tint (the Media Pool's
// slate-for-footage / violet-for-audio semantics, extended with teal for
// transitions). Effect tiles render dimmed — catalogue only, not draggable.
enum class ToolboxKind { Title, Transition, Effect };

// A tile grid whose items are drag sources. Each tile carries its display
// label (DisplayRole), well icon (DecorationRole), drag mime in `Qt::UserRole`
// and the payload in `Qt::UserRole + 1`; items without a mime are not
// draggable. A ToolboxTileDelegate (see the .cpp, mirroring
// MediaPoolTileDelegate) paints every tile — the stylesheet only clears the
// list background. The drag pixmap is a mini card tile so the drop target
// reads the payload as "clip being placed".
//
// The list itself is full-width — the media-pool well (media_pool_style) spans
// the whole tab. With a grid size set and IconMode+Adjust, Qt's icon layout
// spreads the tile columns evenly across the full width, exactly like the
// Media Pool grid above it.
class ToolboxList : public QListWidget {
    Q_OBJECT
public:
    explicit ToolboxList(QWidget* parent = nullptr);

    // Adds one catalogue tile with `text`, its kind, a mono `meta` caption
    // (e.g. "NEW TRACK"), and — when `mime`/`payload` are non-empty — drag
    // behaviour. `icon_name` is the bundled SVG name painted (tinted per kind)
    // into the well; null/unknown falls back to no well icon.
    QListWidgetItem* add_item(const QString& text, ToolboxKind kind, const QString& meta = {},
                              const char* mime = nullptr,
                              const QByteArray& payload = QByteArray(),
                              const char* icon_name = nullptr);

protected:
    void startDrag(Qt::DropActions supported) override;
};

class ToolboxWidget : public QWidget {
    Q_OBJECT
public:
    explicit ToolboxWidget(QWidget* parent = nullptr);

    // Stable title-preset catalogue resident in one place (ids consumed by
    // MainWindow::place_title_at and, later, by the Inspector's title picker).
    struct TitlePreset {
        const char* id = nullptr;
        const char* label = nullptr;   // display name
        const char* sample = nullptr;  // default on-screen text
        float size = 0.0f;             // Core title-layout size law [0.02..0.5]
    };
    static const std::vector<TitlePreset>& title_presets();

private:
    QWidget* build_effects_tab();
    QWidget* build_titles_tab();
    QWidget* build_transitions_tab();
    QWidget* build_more_tab();

    QTabWidget* tabs_ = nullptr;
    ToolboxList* effects_ = nullptr;
    ToolboxList* titles_ = nullptr;
    ToolboxList* transitions_ = nullptr;
};

}  // namespace canvas::gui