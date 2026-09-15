// Subtitles inspector page (see InspectorSubtitles.hpp). Built once per
// MainWindow (registry keyed by window), refreshed from whichever clip is
// selected, enabled only while the selection carries a title overlay.
// Styling edits target EVERY selected caption at once ("Select All Subtitles"
// gathers them into one selection) and every gesture/commit records exactly
// one undo — a GroupCommand wrapping one set_clip_title/set_clip_transform per
// caption — before re-syncing the Video tab's Title row.

#include "UX/InspectorSubtitles.hpp"

#include <QCheckBox>
#include <QColor>
#include <QColorDialog>
#include <QComboBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QSlider>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "UX/InspectorShared.hpp"
#include "UX/InspectorVisual.hpp"
#include "UX/MainWindow.hpp"
#include "UX/theme.hpp"
#include "Widgets/timeline_widget.hpp"
#include "canvas/core/timeline/edit_ops.hpp"
#include "canvas/core/timeline/model.hpp"
#include "canvas/core/timeline/title.hpp"
#include "core/timecode.hpp"

namespace canvas::gui {

namespace {

using canvas::core::Clip;
using canvas::core::Track;

QString muted_label_style() {
    return QStringLiteral("color: %1; font-size: 11px;").arg(css(tokens().ink_muted));
}

QString preview_text_style() {
    return QStringLiteral("color: %1; font-size: 13px; font-weight: 600;")
        .arg(css(tokens().ink));
}

QString combo_style() {
    const ThemeTokens& t = tokens();
    return QStringLiteral(
                "QComboBox { background-color: %1; color: %2; border: 1px solid %3;"
                " border-radius: 8px; padding: 3px 8px; }"
                "QComboBox::drop-down { border: none; width: 18px; }"
                "QComboBox QAbstractItemView { background-color: %4; color: %2;"
                " selection-background-color: %5; border: 1px solid %3;"
                " border-radius: 8px; padding: 2px; }")
        .arg(css(t.surface_higher), css(t.ink), css(t.border), css(t.surface_low),
             css(t.accent));
}

QString zoom_button_style() {
    const ThemeTokens& t = tokens();
    return QStringLiteral(
                "QToolButton { background-color: %1; color: %2; border: 1px solid %3;"
                " border-radius: 8px; padding: 4px 0px; font-size: 14px; font-weight: 700; }"
                "QToolButton:hover { border-color: %4; }"
                "QToolButton:disabled { color: %5; }"
                "QToolButton:checked { background-color: %4; color: %6; border-color: %4; }")
        .arg(css(t.surface_raised), css(t.ink), css(t.border), css(t.accent),
             css(t.ink_faint), css(t.on_accent));
}

// Slider percent <-> size-fraction law (mirrors title::kSizeMin/kSizeMax).
int size_to_percent(const float size) {
    return static_cast<int>(std::lround(
        std::clamp(size, canvas::core::title::kSizeMin, canvas::core::title::kSizeMax) * 100.0f));
}

// Position-slider half-range in output px for the horizontal / up directions.
// The Video tab's Position spins span the full visual law (±4096); the subtitle
// sliders cover the nudge band around centre where captions actually live.
// The DOWN direction grows past this band to reach the frame's bottom edge
// (see update_inspector_subtitles: the slider's lower bound widens to
// half the picture height so any caption can hug the screen edge).
constexpr int kPosRangePx = 550;

// The picture this project draws captions over — the subtitle-positioning and
// caption-fit reference frame. Mirrors timeline_decoder's convention (the first
// real video clip's media wins) with a 1080p fallback.
std::pair<int, int> project_frame_dims(const canvas::core::Project& project) {
    for (const auto& t : project.sequence.video_tracks) {
        for (const auto& c : t.clips) {
            if (c.media < 0) continue;
            const auto* me = project.media_by_id(c.media);
            if (me && me->width > 0 && me->height > 0) return {me->width, me->height};
        }
    }
    return {1920, 1080};
}

// Shadow effect ranges (px / percent). Offset is a nudge band around zero;
// blur 0 = hard edge; opacity is a percent of the tint.
constexpr int kOffsetRangePx = 160;
constexpr int kBlurMaxPx = 32;
constexpr int kOpacityMax = 100;

// Background-box parameters in px: padding ("width"/"height" around the text
// block, the box hugs the text so dimensions grow outward), corner radius.
constexpr int kBoxPadMaxPx = 200;
constexpr int kBoxRadiusMaxPx = 48;

// Bipolar slider readout: "Right 120 px" / "Down 45 px" / "0 px".
QString axis_label(int v, const char* neg, const char* pos) {
    if (v == 0) return QStringLiteral("0 px");
    return QString::fromLatin1(v > 0 ? pos : neg) + QStringLiteral(" ") +
           QString::number(std::abs(v)) + QStringLiteral(" px");
}

// One labelled slider row (offset/blur/opacity/padding): label, live-painted
// slider (one commit per release), value readout.
struct ScalarRow {
    QSlider* slider = nullptr;
    QLabel* value = nullptr;
};

ScalarRow add_scalar_row(QVBoxLayout* body, QWidget* host, const char* label, int min_v,
                         int max_v, int tick, const char* tip) {
    ScalarRow out;
    auto* lay = new QHBoxLayout;
    lay->setSpacing(8);
    auto* lbl = new QLabel(MainWindow::tr(label), host);
    lbl->setMinimumWidth(64);
    apply_theme_style(lbl, &muted_label_style);
    out.slider = new QSlider(Qt::Horizontal, host);
    out.slider->setRange(min_v, max_v);
    out.slider->setTracking(true);  // valueChanged streams during a drag → realtime preview
    out.slider->setTickPosition(QSlider::TicksBelow);
    out.slider->setTickInterval(tick);
    out.slider->setToolTip(MainWindow::tr(tip));
    out.value = new QLabel(QStringLiteral("—"), host);
    out.value->setMinimumWidth(76);
    apply_theme_style(out.value, &muted_label_style);
    lay->addWidget(lbl);
    lay->addWidget(out.slider, 1);
    lay->addWidget(out.value);
    body->addLayout(lay);
    return out;
}

// Colour picker row: a label plus a swatch button that opens QColorDialog
// (click-to-pick beats three R/G/B sliders). The button's fill shows the
// current colour; a click commits the new one as one undoable set_clip_title.
QToolButton* add_color_button(QVBoxLayout* body, QWidget* host, const char* tip) {
    auto* lay = new QHBoxLayout;
    lay->setSpacing(8);
    auto* lbl = new QLabel(MainWindow::tr("Colour"), host);
    lbl->setMinimumWidth(64);
    apply_theme_style(lbl, &muted_label_style);
    auto* btn = new QToolButton(host);
    btn->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    btn->setMinimumHeight(26);
    btn->setCursor(Qt::PointingHandCursor);
    btn->setFocusPolicy(Qt::StrongFocus);
    btn->setToolTip(MainWindow::tr(tip));
    lay->addWidget(lbl);
    lay->addWidget(btn, 1);
    body->addLayout(lay);
    return btn;
}

// Paint a swatch button's fill for an RGB triple (theme-borderfaced).
void paint_color_button(QToolButton* btn, const int r, const int g, const int b) {
    btn->setStyleSheet(QStringLiteral(
                "QToolButton { background-color: rgb(%1, %2, %3); border: 1px solid %4;"
                " border-radius: 7px; }"
                "QToolButton:hover { border-color: %5; }"
                "QToolButton:focus { border-color: %5; }")
                           .arg(r)
                           .arg(g)
                           .arg(b)
                           .arg(css(tokens().border), css(tokens().border_hi)));
}

struct SubControls {
    QLabel* preview = nullptr;
    QLabel* meta = nullptr;
    QToolButton* select_all = nullptr;
    QSlider* size_slider = nullptr;
    QLabel* size_value = nullptr;
    QToolButton* zoom_out = nullptr;
    QToolButton* zoom_in = nullptr;
    QSlider* pos_h = nullptr;  // bipolar px, 0 = centred (right positive)
    QLabel* pos_h_value = nullptr;
    QSlider* pos_v = nullptr;  // bipolar px, 0 = centred (up positive)
    QLabel* pos_v_value = nullptr;
    QToolButton* pos_centre = nullptr;
    QToolButton* style_bold = nullptr;
    QToolButton* style_italic = nullptr;
    QToolButton* style_underline = nullptr;
    QComboBox* font = nullptr;
    QCheckBox* shadow_on = nullptr;
    QSlider* shadow_dx = nullptr;
    QLabel* shadow_dx_value = nullptr;
    QSlider* shadow_dy = nullptr;
    QLabel* shadow_dy_value = nullptr;
    QSlider* shadow_blur = nullptr;
    QLabel* shadow_blur_value = nullptr;
    QSlider* shadow_alpha = nullptr;
    QLabel* shadow_alpha_value = nullptr;
    QToolButton* shadow_color = nullptr;
    QCheckBox* box_on = nullptr;
    QSlider* box_w = nullptr;
    QLabel* box_w_value = nullptr;
    QSlider* box_h = nullptr;
    QLabel* box_h_value = nullptr;
    QSlider* box_radius = nullptr;
    QLabel* box_radius_value = nullptr;
    QSlider* box_alpha = nullptr;
    QLabel* box_alpha_value = nullptr;
    QToolButton* box_color = nullptr;
    bool updating = false;  // guards against committing while populating
    bool attached = false;  // selection signals already connected
    // Realtime drag-gesture state: a slider drag previews every tick through a
    // warm swap_project snapshot and records ONE undo entry on release, with the
    // pre-drag clip as its "before". When several captions are selected the
    // gesture spans them all and the recorded entry is a single GroupCommand.
    struct LiveStart {
        canvas::core::Track::Kind kind = canvas::core::Track::Kind::Video;
        std::size_t track = 0;
        canvas::core::ClipId id = 0;
        Clip clip;  // full pre-drag snapshot of one selected caption
    };
    struct LiveGesture {
        bool active = false;
        bool transform = false;  // pos sliders ride set_clip_transform
        std::vector<LiveStart> starts;  // one snapshot per selected caption
    };
    LiveGesture live;
};

std::map<MainWindow*, SubControls>& sub_registry() {
    static std::map<MainWindow*, SubControls> reg;
    return reg;
}

SubControls* sub_lookup(MainWindow& mw) {
    const auto it = sub_registry().find(&mw);
    return it == sub_registry().end() ? nullptr : &it->second;
}

}  // namespace

void build_inspector_subtitles(MainWindow& mw, QVBoxLayout* subtitles_layout) {
    SubControls& sc = sub_registry()[&mw];
    auto* host = subtitles_layout->parentWidget();
    const auto tr = [](const char* s) { return MainWindow::tr(s); };

    // ---- Caption -----------------------------------------------------------
    auto* caption = new InspectorCategory(tr("Caption"), /*expanded=*/true, host);
    auto* caption_body = caption->body_layout();
    sc.preview = new QLabel(tr("Select a caption or title clip…"), host);
    sc.preview->setWordWrap(true);
    apply_theme_style(sc.preview, &preview_text_style);
    caption_body->addWidget(sc.preview);
    sc.meta = new QLabel(QStringLiteral("—"), host);
    apply_theme_style(sc.meta, &muted_label_style);
    caption_body->addWidget(sc.meta);
    sc.select_all = new QToolButton(host);
    sc.select_all->setText(tr("Select All Subtitles"));
    sc.select_all->setToolTip(tr("Select every caption/title clip so the styling "
                                 "sliders below edit them all at once (one undo)"));
    sc.select_all->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    apply_theme_style(sc.select_all, &zoom_button_style);
    caption_body->addWidget(sc.select_all);
    subtitles_layout->addWidget(caption);

    // ---- Size ---------------------------------------------------------------
    auto* size_cat = new InspectorCategory(tr("Size"), /*expanded=*/true, host);
    auto* size_body = size_cat->body_layout();
    auto* slider_row = new QHBoxLayout;
    slider_row->setSpacing(8);
    sc.size_slider = new QSlider(Qt::Horizontal, host);
    sc.size_slider->setRange(static_cast<int>(canvas::core::title::kSizeMin * 100.0f),
                             static_cast<int>(canvas::core::title::kSizeMax * 100.0f));
    sc.size_slider->setTracking(true);  // valueChanged streams during a drag → realtime preview
    sc.size_slider->setToolTip(tr("Text size as percent of frame height"));
    sc.size_value = new QLabel(QStringLiteral("—"), host);
    sc.size_value->setMinimumWidth(44);
    apply_theme_style(sc.size_value, &muted_label_style);
    slider_row->addWidget(sc.size_slider, 1);
    slider_row->addWidget(sc.size_value);
    size_body->addLayout(slider_row);

    auto* zoom_row = new QHBoxLayout;
    zoom_row->setSpacing(8);
    sc.zoom_out = new QToolButton(host);
    sc.zoom_out->setText(QStringLiteral("\u2212"));  // minus sign
    sc.zoom_out->setToolTip(tr("Zoom text out (shrink 20%)"));
    sc.zoom_out->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    apply_theme_style(sc.zoom_out, &zoom_button_style);
    sc.zoom_in = new QToolButton(host);
    sc.zoom_in->setText(QStringLiteral("+"));
    sc.zoom_in->setToolTip(tr("Zoom text in (enlarge 25%)"));
    sc.zoom_in->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    apply_theme_style(sc.zoom_in, &zoom_button_style);
    zoom_row->addWidget(sc.zoom_out);
    zoom_row->addWidget(sc.zoom_in);
    size_body->addLayout(zoom_row);
    subtitles_layout->addWidget(size_cat);

    // ---- Position ------------------------------------------------------------
    // Two bipolar sliders sharing the clip's visual position (pos_x/pos_y, px)
    // with the Video tab: both rest at 0 = centred. The horizontal slider maps
    // left-to-right; the vertical slider maps bottom(down)-to-top(up).
    auto* pos_cat = new InspectorCategory(tr("Position"), /*expanded=*/true, host);
    auto* pos_body = pos_cat->body_layout();
    const auto pos_label = [&](const char* text) {
        auto* lbl = new QLabel(MainWindow::tr(text), host);
        lbl->setMinimumWidth(64);
        apply_theme_style(lbl, &muted_label_style);
        return lbl;
    };
    auto* h_row = new QHBoxLayout;
    h_row->setSpacing(8);
    sc.pos_h = new QSlider(Qt::Horizontal, host);
    sc.pos_h->setRange(-kPosRangePx, kPosRangePx);
    sc.pos_h->setTracking(true);  // valueChanged streams during a drag → realtime preview
    sc.pos_h->setTickPosition(QSlider::TicksBelow);
    sc.pos_h->setTickInterval(120);
    sc.pos_h->setToolTip(tr("Drag right to move right, left to move left (px)"));
    sc.pos_h_value = new QLabel(QStringLiteral("—"), host);
    sc.pos_h_value->setMinimumWidth(76);
    apply_theme_style(sc.pos_h_value, &muted_label_style);
    h_row->addWidget(pos_label("Left/Right"));
    h_row->addWidget(sc.pos_h, 1);
    h_row->addWidget(sc.pos_h_value);
    pos_body->addLayout(h_row);

    auto* v_row = new QHBoxLayout;
    v_row->setSpacing(8);
    sc.pos_v = new QSlider(Qt::Vertical, host);
    sc.pos_v->setRange(-kPosRangePx, kPosRangePx);
    sc.pos_v->setTracking(true);  // valueChanged streams during a drag → realtime preview
    sc.pos_v->setTickPosition(QSlider::TicksLeft);
    sc.pos_v->setTickInterval(120);
    sc.pos_v->setFixedHeight(110);
    sc.pos_v->setToolTip(tr("Drag up to move up, down to move down (px)"));
    sc.pos_v_value = new QLabel(QStringLiteral("—"), host);
    sc.pos_v_value->setMinimumWidth(76);
    apply_theme_style(sc.pos_v_value, &muted_label_style);
    v_row->addWidget(pos_label("Up/Down"));
    v_row->addWidget(sc.pos_v);
    v_row->addWidget(sc.pos_v_value, 1);
    pos_body->addLayout(v_row);

    sc.pos_centre = new QToolButton(host);
    sc.pos_centre->setText(tr("Centre"));
    sc.pos_centre->setToolTip(tr("Move the caption back to the frame centre"));
    sc.pos_centre->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    apply_theme_style(sc.pos_centre, &zoom_button_style);
    pos_body->addWidget(sc.pos_centre);
    subtitles_layout->addWidget(pos_cat);

    // ---- Style -----------------------------------------------------------------
    auto* style_cat = new InspectorCategory(tr("Style"), /*expanded=*/true, host);
    auto* style_body = style_cat->body_layout();
    auto* style_row = new QHBoxLayout;
    style_row->setSpacing(8);
    const auto style_button = [&](const char* text, const char* tip) {
        auto* btn = new QToolButton(host);
        btn->setText(MainWindow::tr(text));
        btn->setCheckable(true);
        btn->setToolTip(MainWindow::tr(tip));
        btn->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        apply_theme_style(btn, &zoom_button_style);
        style_row->addWidget(btn);
        return btn;
    };
    sc.style_bold = style_button("B", "Bold");
    QFont bold_font = sc.style_bold->font();
    bold_font.setBold(true);
    bold_font.setPointSize(14);
    sc.style_bold->setFont(bold_font);
    sc.style_italic = style_button("I", "Italic");
    QFont italic_font = sc.style_italic->font();
    italic_font.setItalic(true);
    italic_font.setPointSize(14);
    sc.style_italic->setFont(italic_font);
    sc.style_underline = style_button("U", "Underline");
    QFont underline_font = sc.style_underline->font();
    underline_font.setUnderline(true);
    underline_font.setPointSize(14);
    sc.style_underline->setFont(underline_font);
    style_body->addLayout(style_row);
    subtitles_layout->addWidget(style_cat);

    // ---- Shadow ---------------------------------------------------------------
    auto* shadow_cat = new InspectorCategory(tr("Shadow"), /*expanded=*/true, host);
    auto* shadow_body = shadow_cat->body_layout();
    sc.shadow_on = new QCheckBox(tr("Cast Shadow"), host);
    sc.shadow_on->setToolTip(tr("Drop a blurred, tinted copy behind the glyphs"));
    shadow_body->addWidget(sc.shadow_on);
    const ScalarRow sh_dx = add_scalar_row(shadow_body, host, "Offset X", -kOffsetRangePx,
                                           kOffsetRangePx, 40,
                                           "Horizontal shadow offset (px)");
    sc.shadow_dx = sh_dx.slider;
    sc.shadow_dx_value = sh_dx.value;
    const ScalarRow sh_dy = add_scalar_row(shadow_body, host, "Offset Y", -kOffsetRangePx,
                                           kOffsetRangePx, 40,
                                           "Vertical shadow offset (px)");
    sc.shadow_dy = sh_dy.slider;
    sc.shadow_dy_value = sh_dy.value;
    const ScalarRow sh_blur = add_scalar_row(shadow_body, host, "Blur", 0, kBlurMaxPx, 8,
                                             "Softness of the shadow edge (px, 0 = hard)");
    sc.shadow_blur = sh_blur.slider;
    sc.shadow_blur_value = sh_blur.value;
    const ScalarRow sh_alpha = add_scalar_row(shadow_body, host, "Opacity", 0, kOpacityMax,
                                              20, "Strength of the shadow");
sc.shadow_alpha = sh_alpha.slider;
    sc.shadow_alpha_value = sh_alpha.value;
    sc.shadow_color =
        add_color_button(shadow_body, host, "Click to pick the shadow colour");
    subtitles_layout->addWidget(shadow_cat);

    // ---- Box ------------------------------------------------------------------
    // A background box behind the whole text block. "Width"/"Height" add
    // padding around the block (the box hugs the text, so dimensions grow
    // outward); Radius rounds the corners; opacity and colour fill the field.
    auto* box_cat = new InspectorCategory(tr("Box"), /*expanded=*/true, host);
    auto* box_body = box_cat->body_layout();
    sc.box_on = new QCheckBox(tr("Background Box"), host);
    sc.box_on->setToolTip(tr("Fill a box behind the whole caption"));
    box_body->addWidget(sc.box_on);
    const ScalarRow bx_w =
        add_scalar_row(box_body, host, "Width", 0, kBoxPadMaxPx, 40,
                       "Horizontal padding around the text (px)");
    sc.box_w = bx_w.slider;
    sc.box_w_value = bx_w.value;
    const ScalarRow bx_h =
        add_scalar_row(box_body, host, "Height", 0, kBoxPadMaxPx, 20,
                       "Vertical padding around the text (px)");
    sc.box_h = bx_h.slider;
    sc.box_h_value = bx_h.value;
    const ScalarRow bx_radius = add_scalar_row(
        box_body, host, "Radius", 0, kBoxRadiusMaxPx, 12,
        "Corner rounding radius (px, 0 = square box)");
    sc.box_radius = bx_radius.slider;
    sc.box_radius_value = bx_radius.value;
    const ScalarRow bx_alpha = add_scalar_row(box_body, host, "Opacity", 0, kOpacityMax,
                                               20, "Box fill strength");
    sc.box_alpha = bx_alpha.slider;
    sc.box_alpha_value = bx_alpha.value;
    sc.box_color = add_color_button(box_body, host, "Click to pick the box colour");
    subtitles_layout->addWidget(box_cat);

    // ---- Font ----------------------------------------------------------------
    auto* font_cat = new InspectorCategory(tr("Font"), /*expanded=*/true, host);
    auto* font_body = font_cat->body_layout();
    sc.font = new QComboBox(host);
    sc.font->addItem(tr("System Default"), QString());
    for (const std::string& family : canvas::core::title::installed_font_families())
        sc.font->addItem(QString::fromStdString(family), QString::fromStdString(family));
    sc.font->setToolTip(tr("Typeface for this caption (all fonts detected on this system)"));
    apply_theme_style(sc.font, &combo_style);
    font_body->addWidget(sc.font);
    auto* font_hint = new QLabel(
        tr("Unknown or missing families fall back to the system default face."), host);
    font_hint->setWordWrap(true);
    apply_theme_style(font_hint, &muted_label_style);
    font_body->addWidget(font_hint);
    subtitles_layout->addWidget(font_cat);

    // ---- Committing ------------------------------------------------------------
    // Single commit funnel, defined inside the befriended build function (like
    // the File page's commit_metadata) so it can reach the selection/project/
    // undo state: each committed change lands on EVERY selected caption as ONE
    // undoable GroupCommand, then both pages re-sync so the Video tab's Title
    // row never disagrees with this one. "Selected captions" is the intersection
    // of the current selection with the sequence's title clips — audio mates and
    // non-title clips are never touched.
    struct CaptionTarget {
        canvas::core::Track::Kind kind = canvas::core::Track::Kind::Video;
        std::size_t track = 0;
        canvas::core::ClipId id = 0;
        Clip clip;
    };
    const auto caption_targets = [&mw]() {
        std::vector<CaptionTarget> out;
        if (!mw.project_) return out;
        const std::vector<canvas::core::ClipId>& want = mw.selected_clip_ids_;
        const auto wanted = [&want](canvas::core::ClipId id) {
            return std::find(want.begin(), want.end(), id) != want.end();
        };
        std::size_t vi = 0;
        for (const Track& t : mw.project_->sequence.video_tracks) {
            for (const Clip& c : t.clips) {
                if (wanted(c.id) && c.has_title()) {
                    out.push_back({Track::Kind::Video, vi, c.id, c});
                }
            }
            ++vi;
        }
        return out;
    };
    // Live lookup by id (titles live on video tracks only), for the gesture
    // loops that re-fetch each target's live state during/after a drag.
    const auto resolve_caption = [&mw](const canvas::core::ClipId id) -> std::optional<Clip> {
        if (!mw.project_) return std::nullopt;
        for (const Track& t : mw.project_->sequence.video_tracks)
            for (const Clip& c : t.clips)
                if (c.id == id && c.has_title()) return c;
        return std::nullopt;
    };
    const auto commit_with = [&mw, &sc, caption_targets](auto tweak) {
        if (sc.updating) return;
        std::vector<std::unique_ptr<canvas::core::ICommand>> cmds;
        for (const auto& target : caption_targets()) {
            Clip::Title t = target.clip.title;
            tweak(t);
            auto cmd = canvas::core::set_clip_title(mw.project_->sequence, target.kind,
                                                    target.track, target.id, t);
            if (cmd) cmds.push_back(std::move(cmd));
        }
        if (cmds.empty()) return;
        auto group = std::make_unique<canvas::core::GroupCommand>(
            MainWindow::tr("Edit Captions").toStdString(), std::move(cmds));
        mw.undo_.record(std::move(group));
        mw.has_unsaved_changes_ = true;
        mw.push_snapshot();
        mw.refresh_timeline();
        update_inspector_visual(mw);
        update_inspector_subtitles(mw);
    };

    // --- Realtime slider gestures --------------------------------------------
    // The Size/Position/Shadow/Box sliders are realtime: sliderPressed stashes
    // the pre-drag clip, valueChanged applies the live change and pushes a warm
    // swap_project snapshot (the current frame re-presents through cached
    // decoders at mouse-move cadence instead of set_project()'s ~217ms decode
    // teardown), and sliderReleased records exactly ONE undo entry — reverting
    // the model to the stashed start through the op, then re-applying the final
    // value, so the recorded command's "before" brackets the whole gesture.
    const auto gesture_press = [&mw, &sc, caption_targets](const bool transform) {
        if (sc.updating) return;
        sc.live.active = false;
        sc.live.starts.clear();
        const std::vector<CaptionTarget> targets = caption_targets();
        if (targets.empty()) return;
        sc.live.active = true;
        sc.live.transform = transform;
        for (const auto& target : targets)
            sc.live.starts.push_back({target.kind, target.track, target.id, target.clip});
    };
    const auto gesture_tweak = [&mw, &sc, resolve_caption](const std::function<void(Clip&)>& tweak) {
        if (!sc.live.active) return;
        for (const auto& start : sc.live.starts) {
            std::optional<Clip> cur = resolve_caption(start.id);
            if (!cur || !cur->has_title()) continue;
            tweak(*cur);
            if (sc.live.transform) {
                auto cmd = canvas::core::set_clip_transform(
                    mw.project_->sequence, start.kind, start.track, start.id, cur->scale_x,
                    cur->scale_y, cur->pos_x, cur->pos_y, cur->rotation_deg, cur->anchor_dx,
                    cur->anchor_dy, cur->flip_h, cur->flip_v);
                if (!cmd) continue;
            } else {
                auto cmd = canvas::core::set_clip_title(mw.project_->sequence, start.kind,
                                                        start.track, start.id, cur->title);
                if (!cmd) continue;
            }
        }
        mw.has_unsaved_changes_ = true;
        mw.push_live_snapshot();  // re-present current frame, decoders stay warm
        update_inspector_visual(mw);
        update_inspector_subtitles(mw);
    };
    const auto gesture_end = [&mw, &sc, resolve_caption]() -> void {
        if (!sc.live.active) return;
        sc.live.active = false;
        if (sc.live.starts.empty()) return;
        std::vector<std::unique_ptr<canvas::core::ICommand>> cmds;
        for (const auto& start : sc.live.starts) {
            std::optional<Clip> cur = resolve_caption(start.id);
            if (!cur || !cur->has_title()) continue;
            if (sc.live.transform) {
                const bool same = start.clip.scale_x == cur->scale_x &&
                                  start.clip.scale_y == cur->scale_y &&
                                  start.clip.pos_x == cur->pos_x && start.clip.pos_y == cur->pos_y &&
                                  start.clip.rotation_deg == cur->rotation_deg &&
                                  start.clip.anchor_dx == cur->anchor_dx &&
                                  start.clip.anchor_dy == cur->anchor_dy &&
                                  start.clip.flip_h == cur->flip_h &&
                                  start.clip.flip_v == cur->flip_v;
                if (same) continue;  // dragged back to the start — nothing to record
                // Revert then re-apply: each recorded command's "before" is that
                // clip's pre-gesture state, so one undo reverts the whole gesture.
                canvas::core::set_clip_transform(
                    mw.project_->sequence, start.kind, start.track, start.id, start.clip.scale_x,
                    start.clip.scale_y, start.clip.pos_x, start.clip.pos_y,
                    start.clip.rotation_deg, start.clip.anchor_dx, start.clip.anchor_dy,
                    start.clip.flip_h, start.clip.flip_v);
                auto cmd = canvas::core::set_clip_transform(
                    mw.project_->sequence, start.kind, start.track, start.id, cur->scale_x,
                    cur->scale_y, cur->pos_x, cur->pos_y, cur->rotation_deg, cur->anchor_dx,
                    cur->anchor_dy, cur->flip_h, cur->flip_v);
                if (cmd) cmds.push_back(std::move(cmd));
            } else {
                if (start.clip.title == cur->title) continue;  // dragged back — no change
                canvas::core::set_clip_title(mw.project_->sequence, start.kind, start.track,
                                             start.id, start.clip.title);
                auto cmd = canvas::core::set_clip_title(mw.project_->sequence, start.kind,
                                                        start.track, start.id, cur->title);
                if (cmd) cmds.push_back(std::move(cmd));
            }
        }
        if (cmds.empty()) return;  // whole drag netted zero change
        auto group = std::make_unique<canvas::core::GroupCommand>(
            MainWindow::tr("Edit Captions").toStdString(), std::move(cmds));
        mw.undo_.record(std::move(group));
        mw.has_unsaved_changes_ = true;
        mw.push_snapshot();
        mw.refresh_timeline();
        update_inspector_visual(mw);
        update_inspector_subtitles(mw);
    };

    QObject::connect(sc.size_slider, &QSlider::sliderPressed, &mw,
                     [gesture_press]() { gesture_press(false); });
    QObject::connect(sc.size_slider, &QSlider::valueChanged, &mw,
                     [gesture_tweak, &sc](int percent) {
                         sc.size_value->setText(QStringLiteral("%1%").arg(percent));
                         if (sc.updating) return;
                         gesture_tweak([percent](Clip& c) {
                             c.title.size = static_cast<float>(percent) / 100.0f;
                         });
                     });
    QObject::connect(sc.size_slider, &QSlider::sliderReleased, &mw,
                     [gesture_end]() { gesture_end(); });
    const auto zoom_commit = [commit_with](float factor) {
        commit_with([factor](Clip::Title& t) { t.size = t.size * factor; });
    };
    QObject::connect(sc.zoom_out, &QToolButton::clicked, &mw,
                     [zoom_commit]() { zoom_commit(0.8f); });
    QObject::connect(sc.zoom_in, &QToolButton::clicked, &mw,
                     [zoom_commit]() { zoom_commit(1.25f); });
    QObject::connect(sc.font, qOverload<int>(&QComboBox::currentIndexChanged), &mw,
                     [commit_with, &sc]() {
                         commit_with([&sc](Clip::Title& t) {
                             t.font_family =
                                 sc.font->currentData().toString().toStdString();
                         });
                     });

    // Position commits ride the transform op (pos lives on the Clip, not the
    // Title), carrying each clip's other transform fields through untouched.
    const auto commit_pos = [&mw, &sc, caption_targets](const double px, const double py) {
        if (sc.updating) return;
        std::vector<std::unique_ptr<canvas::core::ICommand>> cmds;
        for (const auto& target : caption_targets()) {
            const Clip& c = target.clip;
            auto cmd = canvas::core::set_clip_transform(
                mw.project_->sequence, target.kind, target.track, target.id, c.scale_x, c.scale_y,
                px, py, c.rotation_deg, c.anchor_dx, c.anchor_dy, c.flip_h, c.flip_v);
            if (cmd) cmds.push_back(std::move(cmd));
        }
        if (cmds.empty()) return;
        auto group = std::make_unique<canvas::core::GroupCommand>(
            MainWindow::tr("Edit Captions").toStdString(), std::move(cmds));
        mw.undo_.record(std::move(group));
        mw.has_unsaved_changes_ = true;
        mw.push_snapshot();
        mw.refresh_timeline();
        update_inspector_visual(mw);     // Video tab Position spins follow
        update_inspector_subtitles(mw);  // sliders follow the clamped result
    };
    QObject::connect(sc.pos_h, &QSlider::sliderPressed, &mw,
                     [gesture_press]() { gesture_press(true); });
    QObject::connect(sc.pos_h, &QSlider::valueChanged, &mw,
                     [gesture_tweak, &sc](int v) {
                         sc.pos_h_value->setText(axis_label(v, "Left", "Right"));
                         if (sc.updating) return;
                         gesture_tweak([v](Clip& c) { c.pos_x = static_cast<double>(v); });
                     });
    QObject::connect(sc.pos_h, &QSlider::sliderReleased, &mw,
                     [gesture_end]() { gesture_end(); });
    QObject::connect(sc.pos_v, &QSlider::sliderPressed, &mw,
                     [gesture_press]() { gesture_press(true); });
    QObject::connect(sc.pos_v, &QSlider::valueChanged, &mw,
                     [gesture_tweak, &sc](int v) {
                         sc.pos_v_value->setText(axis_label(v, "Down", "Up"));
                         if (sc.updating) return;
                         // Slider up is positive; screen y grows downward.
                         gesture_tweak([v](Clip& c) { c.pos_y = -static_cast<double>(v); });
                     });
    QObject::connect(sc.pos_v, &QSlider::sliderReleased, &mw,
                     [gesture_end]() { gesture_end(); });
    QObject::connect(sc.pos_centre, &QToolButton::clicked, &mw,
                     [commit_pos]() { commit_pos(0.0, 0.0); });

    // Select-all-captions: gathers every title clip on the video tracks into one
    // selection so the styling controls above edit them all at once. Mirrors the
    // widget's range-select plumbing (selection round-trips through the widget so
    // linked audio mates ride along; caption_targets filters them out later).
    QObject::connect(sc.select_all, &QToolButton::clicked, &mw, [&mw, &sc]() {
        if (!mw.project_) return;
        std::vector<canvas::core::ClipId> ids;
        for (const Track& t : mw.project_->sequence.video_tracks)
            for (const Clip& c : t.clips)
                if (c.has_title()) ids.push_back(c.id);
        if (ids.empty()) return;
        mw.timeline()->set_selection(ids);
        mw.selected_clip_ = ids.front();
        mw.selected_clip_ids_ = mw.timeline()->selected_clip_ids();
        update_inspector_visual(mw);
        update_inspector_subtitles(mw);
    });

    QObject::connect(sc.style_bold, &QToolButton::toggled, &mw,
                     [commit_with](bool on) {
                         commit_with([on](Clip::Title& t) { t.bold = on; });
                     });
    QObject::connect(sc.style_italic, &QToolButton::toggled, &mw,
                     [commit_with](bool on) {
                         commit_with([on](Clip::Title& t) { t.italic = on; });
                     });
    QObject::connect(sc.style_underline, &QToolButton::toggled, &mw,
                     [commit_with](bool on) {
                         commit_with([on](Clip::Title& t) { t.underline = on; });
                     });

    // --- Shadow commits ------------------------------------------------------
    QObject::connect(sc.shadow_on, &QCheckBox::toggled, &mw,
                     [commit_with](bool on) { commit_with([on](Clip::Title& t) { t.shadow = on; }); });
    QObject::connect(sc.shadow_dx, &QSlider::sliderPressed, &mw,
                     [gesture_press]() { gesture_press(false); });
    QObject::connect(sc.shadow_dx, &QSlider::valueChanged, &mw,
                     [gesture_tweak, &sc](int v) {
                         sc.shadow_dx_value->setText(axis_label(v, "Left", "Right"));
                         if (sc.updating) return;
                         gesture_tweak([v](Clip& c) { c.title.shadow_dx = static_cast<float>(v); });
                     });
    QObject::connect(sc.shadow_dx, &QSlider::sliderReleased, &mw,
                     [gesture_end]() { gesture_end(); });
    QObject::connect(sc.shadow_dy, &QSlider::sliderPressed, &mw,
                     [gesture_press]() { gesture_press(false); });
    QObject::connect(sc.shadow_dy, &QSlider::valueChanged, &mw,
                     [gesture_tweak, &sc](int v) {
                         sc.shadow_dy_value->setText(axis_label(v, "Up", "Down"));
                         if (sc.updating) return;
                         gesture_tweak([v](Clip& c) { c.title.shadow_dy = static_cast<float>(v); });
                     });
    QObject::connect(sc.shadow_dy, &QSlider::sliderReleased, &mw,
                     [gesture_end]() { gesture_end(); });
    QObject::connect(sc.shadow_blur, &QSlider::sliderPressed, &mw,
                     [gesture_press]() { gesture_press(false); });
    QObject::connect(sc.shadow_blur, &QSlider::valueChanged, &mw,
                     [gesture_tweak, &sc](int v) {
                         sc.shadow_blur_value->setText(QStringLiteral("%1 px").arg(v));
                         if (sc.updating) return;
                         gesture_tweak([v](Clip& c) { c.title.shadow_blur = static_cast<float>(v); });
                     });
    QObject::connect(sc.shadow_blur, &QSlider::sliderReleased, &mw,
                     [gesture_end]() { gesture_end(); });
    QObject::connect(sc.shadow_alpha, &QSlider::sliderPressed, &mw,
                     [gesture_press]() { gesture_press(false); });
    QObject::connect(sc.shadow_alpha, &QSlider::valueChanged, &mw,
                     [gesture_tweak, &sc](int v) {
                         sc.shadow_alpha_value->setText(QStringLiteral("%1%").arg(v));
                         if (sc.updating) return;
                         gesture_tweak([v](Clip& c) {
                             c.title.shadow_opacity = static_cast<float>(v) / 100.0f;
                         });
                     });
    QObject::connect(sc.shadow_alpha, &QSlider::sliderReleased, &mw,
                     [gesture_end]() { gesture_end(); });

    // --- Box commits ---------------------------------------------------------
    QObject::connect(sc.box_on, &QCheckBox::toggled, &mw,
                     [commit_with](bool on) { commit_with([on](Clip::Title& t) { t.box = on; }); });
    QObject::connect(sc.box_w, &QSlider::sliderPressed, &mw,
                     [gesture_press]() { gesture_press(false); });
    QObject::connect(sc.box_w, &QSlider::valueChanged, &mw,
                     [gesture_tweak, &sc](int v) {
                         sc.box_w_value->setText(QStringLiteral("%1 px").arg(v));
                         if (sc.updating) return;
                         gesture_tweak([v](Clip& c) { c.title.box_pad_x = static_cast<float>(v); });
                     });
    QObject::connect(sc.box_w, &QSlider::sliderReleased, &mw,
                     [gesture_end]() { gesture_end(); });
    QObject::connect(sc.box_h, &QSlider::sliderPressed, &mw,
                     [gesture_press]() { gesture_press(false); });
    QObject::connect(sc.box_h, &QSlider::valueChanged, &mw,
                     [gesture_tweak, &sc](int v) {
                         sc.box_h_value->setText(QStringLiteral("%1 px").arg(v));
                         if (sc.updating) return;
                         gesture_tweak([v](Clip& c) { c.title.box_pad_y = static_cast<float>(v); });
                     });
    QObject::connect(sc.box_h, &QSlider::sliderReleased, &mw,
                     [gesture_end]() { gesture_end(); });
    QObject::connect(sc.box_radius, &QSlider::sliderPressed, &mw,
                     [gesture_press]() { gesture_press(false); });
    QObject::connect(sc.box_radius, &QSlider::valueChanged, &mw,
                     [gesture_tweak, &sc](int v) {
                         sc.box_radius_value->setText(QStringLiteral("%1 px").arg(v));
                         if (sc.updating) return;
                         gesture_tweak([v](Clip& c) { c.title.box_radius = static_cast<float>(v); });
                     });
    QObject::connect(sc.box_radius, &QSlider::sliderReleased, &mw,
                     [gesture_end]() { gesture_end(); });
    QObject::connect(sc.box_alpha, &QSlider::sliderPressed, &mw,
                     [gesture_press]() { gesture_press(false); });
    QObject::connect(sc.box_alpha, &QSlider::valueChanged, &mw,
                     [gesture_tweak, &sc](int v) {
                         sc.box_alpha_value->setText(QStringLiteral("%1%").arg(v));
                         if (sc.updating) return;
                         gesture_tweak([v](Clip& c) {
                             c.title.box_opacity = static_cast<float>(v) / 100.0f;
                         });
                     });
    QObject::connect(sc.box_alpha, &QSlider::sliderReleased, &mw,
                     [gesture_end]() { gesture_end(); });

    // Colour pickers: one QColorDialog commit per click, seed = the current
    // tint so reopening the dialog lands where the user last left it. The picked
    // colour lands on every selected caption as one GroupCommand.
    const auto pick_color =
        [&mw, &sc, caption_targets](
            const std::function<void(Clip::Title&, const QColor&)>& apply,
            const std::function<QColor(const Clip&)>& seed) {
            if (sc.updating) return;
            QColor initial = Qt::white;
            {
                const std::vector<CaptionTarget> targets = caption_targets();
                if (!targets.empty()) initial = seed(targets.front().clip);
            }
            const QColor chosen =
                QColorDialog::getColor(initial, &mw, MainWindow::tr("Choose colour"));
            if (!chosen.isValid()) return;
            std::vector<std::unique_ptr<canvas::core::ICommand>> cmds;
            for (const auto& target : caption_targets()) {
                Clip::Title t = target.clip.title;
                apply(t, chosen);
                auto cmd = canvas::core::set_clip_title(mw.project_->sequence, target.kind,
                                                        target.track, target.id, t);
                if (cmd) cmds.push_back(std::move(cmd));
            }
            if (cmds.empty()) return;
            auto group = std::make_unique<canvas::core::GroupCommand>(
                MainWindow::tr("Edit Captions").toStdString(), std::move(cmds));
            mw.undo_.record(std::move(group));
            mw.has_unsaved_changes_ = true;
            mw.push_snapshot();
            mw.refresh_timeline();
            update_inspector_visual(mw);
            update_inspector_subtitles(mw);
        };
    QObject::connect(sc.shadow_color, &QToolButton::clicked, &mw,
                     [pick_color] {
                         pick_color(
                             [](Clip::Title& t, const QColor& c) {
                                 t.shadow_r = static_cast<float>(c.redF());
                                 t.shadow_g = static_cast<float>(c.greenF());
                                 t.shadow_b = static_cast<float>(c.blueF());
                             },
                             [](const Clip& clip) {
                                 return QColor::fromRgbF(clip.title.shadow_r, clip.title.shadow_g,
                                                         clip.title.shadow_b);
                             });
                     });
    QObject::connect(sc.box_color, &QToolButton::clicked, &mw,
                     [pick_color] {
                         pick_color(
                             [](Clip::Title& t, const QColor& c) {
                                 t.box_r = static_cast<float>(c.redF());
                                 t.box_g = static_cast<float>(c.greenF());
                                 t.box_b = static_cast<float>(c.blueF());
                             },
                             [](const Clip& clip) {
                                 return QColor::fromRgbF(clip.title.box_r, clip.title.box_g,
                                                         clip.title.box_b);
                             });
                     });
}

void attach_inspector_subtitles(MainWindow& mw, TimelineWidget* timeline) {
    SubControls* sc = sub_lookup(mw);
    if (!sc || !timeline || sc->attached) return;
    sc->attached = true;
    QObject::connect(timeline, &TimelineWidget::clip_selected, &mw,
                     [&mw](const canvas::core::Clip*) { update_inspector_subtitles(mw); });
    QObject::connect(timeline, &TimelineWidget::clips_range_selected, &mw,
                     [&mw](std::vector<canvas::core::ClipId>) {
                         update_inspector_subtitles(mw);
                     });
}

void update_inspector_subtitles(MainWindow& mw) {
    SubControls* sc = sub_lookup(mw);
    if (!sc || !sc->size_slider || !mw.project_) return;

    Track::Kind kind;
    std::size_t index = 0;
    Clip clip;
    const bool has = mw.find_selected_clip(kind, index, clip) && clip.has_title();

    // The vertical travel must reach the bottom EDGE of the picture, so the
    // slider's down bound (negative side: slider-down = screen-down) widens
    // with the frame height — half the height puts even a tiny caption's top at
    // the screen edge. Up stays within the 550px nudge band.
    const int frame_h = project_frame_dims(*mw.project_).second;
    const int down_max = std::max(kPosRangePx, frame_h / 2);
    sc->pos_v->setRange(-down_max, kPosRangePx);

    QWidget* enable_set[] = {sc->preview, sc->meta, sc->size_slider, sc->size_value,
                             sc->zoom_out, sc->zoom_in, sc->pos_h, sc->pos_h_value,
                             sc->pos_v, sc->pos_v_value, sc->pos_centre,
                             sc->style_bold, sc->style_italic, sc->style_underline,
                             sc->font,
                             sc->shadow_on, sc->shadow_dx, sc->shadow_dx_value,
                             sc->shadow_dy, sc->shadow_dy_value,
                             sc->shadow_blur, sc->shadow_blur_value,
                             sc->shadow_alpha, sc->shadow_alpha_value,
                             sc->shadow_color,
                             sc->box_on, sc->box_w, sc->box_w_value,
                             sc->box_h, sc->box_h_value,
                             sc->box_radius, sc->box_radius_value,
                             sc->box_alpha, sc->box_alpha_value,
                             sc->box_color};
    for (QWidget* w : enable_set)
        if (w) w->setEnabled(has);
    if (sc->select_all) {
        bool any_caption = false;
        for (const Track& t : mw.project_->sequence.video_tracks)
            for (const Clip& c : t.clips)
                if (c.has_title()) {
                    any_caption = true;
                    break;
                }
        sc->select_all->setEnabled(any_caption);
    }
    if (!has) {
        sc->preview->setText(MainWindow::tr("Select a caption or title clip…"));
        sc->meta->setText(QStringLiteral("—"));
        return;
    }

    QString text = QString::fromStdString(clip.title.text).replace(QLatin1Char('\n'),
                                                                   QStringLiteral(" ⏎ "));
    if (text.size() > 120) text = text.left(120) + QStringLiteral("…");
    sc->preview->setText(text);
    const double fps = mw.project_->sequence.fps;
    sc->meta->setText(QStringLiteral("%1 → %2 · %3%4")
                          .arg(timecode(clip.tl_in, fps), timecode(clip.tl_out, fps),
                               kind == Track::Kind::Video ? QStringLiteral("V")
                                                          : QStringLiteral("A"))
                          .arg(index + 1));
    const std::vector<canvas::core::ClipId>& wanted = mw.selected_clip_ids_;
    int multi = 0;
    for (const Track& t : mw.project_->sequence.video_tracks)
        for (const Clip& c : t.clips)
            if (c.has_title() && std::find(wanted.begin(), wanted.end(), c.id) != wanted.end())
                ++multi;
    if (multi > 1) {
        sc->meta->setText(sc->meta->text() +
                          QStringLiteral(" · edits apply to %1 captions").arg(multi));
    }

    sc->updating = true;
    sc->size_slider->setValue(size_to_percent(clip.title.size));
    sc->size_value->setText(QStringLiteral("%1%").arg(size_to_percent(clip.title.size)));
    const int hv = std::clamp(static_cast<int>(std::lround(clip.pos_x)), -kPosRangePx,
                              kPosRangePx);
    sc->pos_h->setValue(hv);
    sc->pos_h_value->setText(axis_label(hv, "Left", "Right"));
    // Slider up is positive; screen y grows downward.
    const int vv = std::clamp(static_cast<int>(std::lround(-clip.pos_y)),
                              sc->pos_v->minimum(), sc->pos_v->maximum());
    sc->pos_v->setValue(vv);
    sc->pos_v_value->setText(axis_label(vv, "Down", "Up"));
    sc->style_bold->setChecked(clip.title.bold);
    sc->style_italic->setChecked(clip.title.italic);
    sc->style_underline->setChecked(clip.title.underline);
    const int font_idx =
        sc->font->findData(QString::fromStdString(clip.title.font_family));
    sc->font->setCurrentIndex(font_idx >= 0 ? font_idx : 0);

    // Shadow effect state.
    sc->shadow_on->setChecked(clip.title.shadow);
    const int sdx = std::clamp(static_cast<int>(std::lround(clip.title.shadow_dx)),
                               -kOffsetRangePx, kOffsetRangePx);
    sc->shadow_dx->setValue(sdx);
    sc->shadow_dx_value->setText(axis_label(sdx, "Left", "Right"));
    const int sdy = std::clamp(static_cast<int>(std::lround(clip.title.shadow_dy)),
                               -kOffsetRangePx, kOffsetRangePx);
    sc->shadow_dy->setValue(sdy);
    sc->shadow_dy_value->setText(axis_label(sdy, "Up", "Down"));
    const int sblur = std::clamp(static_cast<int>(std::lround(clip.title.shadow_blur)), 0,
                                 kBlurMaxPx);
    sc->shadow_blur->setValue(sblur);
    sc->shadow_blur_value->setText(QStringLiteral("%1 px").arg(sblur));
    const int sale = std::clamp(
        static_cast<int>(std::lround(clip.title.shadow_opacity * 100.0f)), 0, kOpacityMax);
    sc->shadow_alpha->setValue(sale);
    sc->shadow_alpha_value->setText(QStringLiteral("%1%").arg(sale));
    const int shr = std::clamp(static_cast<int>(std::lround(clip.title.shadow_r * 255.0f)),
                               0, 255);
    const int shg = std::clamp(static_cast<int>(std::lround(clip.title.shadow_g * 255.0f)),
                               0, 255);
    const int shb = std::clamp(static_cast<int>(std::lround(clip.title.shadow_b * 255.0f)),
                               0, 255);
    paint_color_button(sc->shadow_color, shr, shg, shb);

    // Background box state.
    sc->box_on->setChecked(clip.title.box);
    const int bw = std::clamp(static_cast<int>(std::lround(clip.title.box_pad_x)), 0,
                              kBoxPadMaxPx);
    sc->box_w->setValue(bw);
    sc->box_w_value->setText(QStringLiteral("%1 px").arg(bw));
    const int bh = std::clamp(static_cast<int>(std::lround(clip.title.box_pad_y)), 0,
                              kBoxPadMaxPx);
    sc->box_h->setValue(bh);
    sc->box_h_value->setText(QStringLiteral("%1 px").arg(bh));
    const int brad = std::clamp(static_cast<int>(std::lround(clip.title.box_radius)), 0,
                                kBoxRadiusMaxPx);
    sc->box_radius->setValue(brad);
    sc->box_radius_value->setText(QStringLiteral("%1 px").arg(brad));
    const int bale = std::clamp(
        static_cast<int>(std::lround(clip.title.box_opacity * 100.0f)), 0, kOpacityMax);
    sc->box_alpha->setValue(bale);
    sc->box_alpha_value->setText(QStringLiteral("%1%").arg(bale));
    const int bxr = std::clamp(static_cast<int>(std::lround(clip.title.box_r * 255.0f)), 0,
                               255);
    const int bxg = std::clamp(static_cast<int>(std::lround(clip.title.box_g * 255.0f)), 0,
                               255);
    const int bxb = std::clamp(static_cast<int>(std::lround(clip.title.box_b * 255.0f)), 0,
                               255);
    paint_color_button(sc->box_color, bxr, bxg, bxb);
    sc->updating = false;
}

void apply_inspector_subtitles(MainWindow& mw) {
    (void)mw;  // commits are event-driven from the control signals
}

}  // namespace canvas::gui