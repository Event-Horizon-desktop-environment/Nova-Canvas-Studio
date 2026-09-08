#include "UX/InspectorVisual.hpp"

#include "UX/InspectorShared.hpp"
#include "UX/MainWindow.hpp"

#include "canvas/core/timeline/edit_ops.hpp"
#include "canvas/core/timeline/visual.hpp"

#include <QComboBox>
#include <QDoubleSpinBox>
#include <QLabel>
#include <QSlider>
#include <QToolButton>

#include <map>
#include <memory>
#include <utility>

#include "Widgets/timeline_widget.hpp"

namespace canvas::gui {

namespace {

struct VisualControls {
    QDoubleSpinBox* zoom_x = nullptr;
    QDoubleSpinBox* zoom_y = nullptr;
    QDoubleSpinBox* pos_x = nullptr;
    QDoubleSpinBox* pos_y = nullptr;
    QDoubleSpinBox* rotation = nullptr;
    QDoubleSpinBox* anchor_x = nullptr;
    QDoubleSpinBox* anchor_y = nullptr;
    QToolButton* flip_h = nullptr;
    QToolButton* flip_v = nullptr;
    QToolButton* chain = nullptr;
    QComboBox* blend = nullptr;
    QSlider* opacity_slider = nullptr;
    QLabel* opacity_value = nullptr;
    bool updating = false;   // guards against commit/re-sync during refresh
    bool attached = false;   // selection signals already connected
};

std::map<MainWindow*, VisualControls>& visual_registry() {
    static std::map<MainWindow*, VisualControls> reg;
    return reg;
}

VisualControls* lookup(MainWindow& mw) {
    const auto it = visual_registry().find(&mw);
    return it == visual_registry().end() ? nullptr : &it->second;
}

}  // namespace

void build_inspector_visual(MainWindow& mw, QVBoxLayout* video_layout) {
    VisualControls& vc = visual_registry()[&mw];
    auto* host = video_layout->parentWidget();
    const auto tr = [&](const char* s) { return MainWindow::tr(s); };

    // --- Transform ----------------------------------------------------------
    auto* transform = new InspectorCategory(tr("Transform"), true, host);
    auto* zoom_row = new QWidget(host);
    auto* zoom_layout = new QHBoxLayout(zoom_row);
    zoom_layout->setContentsMargins(0, 0, 0, 0);
    zoom_layout->setSpacing(4);
    vc.zoom_x = make_numeric(canvas::core::visual::kScaleMin, canvas::core::visual::kScaleMax,
                             canvas::core::visual::kScaleDefault, zoom_row);
    vc.chain = new QToolButton(zoom_row);
    vc.chain->setIcon(icon("chain"));
    vc.chain->setIconSize(QSize(14, 14));
    vc.chain->setCheckable(true);
    vc.chain->setChecked(true);
    vc.chain->setToolTip(tr("Link X and Y"));
    vc.zoom_y = make_numeric(canvas::core::visual::kScaleMin, canvas::core::visual::kScaleMax,
                             canvas::core::visual::kScaleDefault, zoom_row);
    zoom_layout->addWidget(vc.zoom_x);
    zoom_layout->addWidget(vc.chain);
    zoom_layout->addWidget(vc.zoom_y);
    add_property_row(transform->body_layout(), tr("Zoom"), zoom_row);

    auto* pos_row = new QWidget(host);
    auto* pos_layout = new QHBoxLayout(pos_row);
    pos_layout->setContentsMargins(0, 0, 0, 0);
    pos_layout->setSpacing(4);
    vc.pos_x = make_numeric(canvas::core::visual::kPosMin, canvas::core::visual::kPosMax,
                            canvas::core::visual::kPosDefault, pos_row);
    vc.pos_y = make_numeric(canvas::core::visual::kPosMin, canvas::core::visual::kPosMax,
                            canvas::core::visual::kPosDefault, pos_row);
    pos_layout->addWidget(vc.pos_x);
    pos_layout->addWidget(vc.pos_y);
    add_property_row(transform->body_layout(), tr("Position"), pos_row);

    vc.rotation = make_numeric(canvas::core::visual::kRotationMin,
                               canvas::core::visual::kRotationMax,
                               canvas::core::visual::kRotationDefault, host);
    add_property_row(transform->body_layout(), tr("Rotation Angle"), vc.rotation);

    auto* anchor_row = new QWidget(host);
    auto* anchor_layout = new QHBoxLayout(anchor_row);
    anchor_layout->setContentsMargins(0, 0, 0, 0);
    anchor_layout->setSpacing(4);
    vc.anchor_x = make_numeric(canvas::core::visual::kAnchorMin, canvas::core::visual::kAnchorMax,
                               canvas::core::visual::kAnchorDefault, anchor_row);
    vc.anchor_y = make_numeric(canvas::core::visual::kAnchorMin, canvas::core::visual::kAnchorMax,
                               canvas::core::visual::kAnchorDefault, anchor_row);
    anchor_layout->addWidget(vc.anchor_x);
    anchor_layout->addWidget(vc.anchor_y);
    add_property_row(transform->body_layout(), tr("Anchor Point"), anchor_row);

    // Pitch/Yaw are reference rows with no backing model fields yet — kept but
    // intentionally unwired (a 3D rotate is out of the 2D transform scope).
    add_property_row(transform->body_layout(), tr("Pitch"),
                     make_numeric(-180.0, 180.0, 0.0, host));
    add_property_row(transform->body_layout(), tr("Yaw"),
                     make_numeric(-180.0, 180.0, 0.0, host));

    auto* flip_row = new QWidget(host);
    auto* flip_layout = new QHBoxLayout(flip_row);
    flip_layout->setContentsMargins(0, 0, 0, 0);
    flip_layout->setSpacing(4);
    vc.flip_h = new QToolButton(flip_row);
    vc.flip_h->setIcon(icon("flip_h"));
    vc.flip_h->setIconSize(QSize(14, 14));
    vc.flip_h->setCheckable(true);
    vc.flip_h->setToolTip(tr("Flip Horizontal"));
    vc.flip_v = new QToolButton(flip_row);
    vc.flip_v->setIcon(icon("flip_v"));
    vc.flip_v->setIconSize(QSize(14, 14));
    vc.flip_v->setCheckable(true);
    vc.flip_v->setToolTip(tr("Flip Vertical"));
    flip_layout->addWidget(vc.flip_h);
    flip_layout->addWidget(vc.flip_v);
    flip_layout->addStretch(1);
    add_property_row(transform->body_layout(), tr("Flip"), flip_row, /*with_reset=*/false);
    video_layout->addWidget(transform);

    // --- Cropping / Dynamic Zoom (reference placeholders, unwired) ------------
    video_layout->addWidget(new InspectorCategory(tr("Cropping"), false, host));
    video_layout->addWidget(new InspectorCategory(tr("Dynamic Zoom"), false, host));

    // --- Composite -----------------------------------------------------------
    auto* composite = new InspectorCategory(tr("Composite"), false, host);
    vc.blend = new QComboBox(host);
    vc.blend->addItems({tr("Normal"), tr("Add"), tr("Multiply"), tr("Screen"), tr("Overlay")});
    add_property_row(composite->body_layout(), tr("Composite Mode"), vc.blend);
    auto* opacity_row = new QWidget(host);
    auto* opacity_layout = new QHBoxLayout(opacity_row);
    opacity_layout->setContentsMargins(0, 0, 0, 0);
    opacity_layout->setSpacing(4);
    vc.opacity_slider = new QSlider(Qt::Horizontal, opacity_row);
    vc.opacity_slider->setRange(0, 100);
    vc.opacity_slider->setValue(100);
    // Compressible so the row fits narrow inspector widths / high DPI.
    vc.opacity_slider->setMinimumWidth(0);
    vc.opacity_value = new QLabel(QStringLiteral("1.00"), opacity_row);
    apply_theme_style(vc.opacity_value, [] {
        return QStringLiteral("color: %1; font-size: 11px;").arg(css(tokens().ink));
    });
    opacity_layout->addWidget(vc.opacity_slider, 1);
    opacity_layout->addWidget(vc.opacity_value);
    add_property_row(composite->body_layout(), tr("Opacity"), opacity_row);
    video_layout->addWidget(composite);

    for (const char* name : {"Speed Change", "Stabilization", "Lens Correction",
                             "Retime and Scaling"}) {
        video_layout->addWidget(new InspectorCategory(tr(name), false, host));
    }

    // ---- Wiring --------------------------------------------------------------
    // Editing the X or Y zoom spin mirrors the linked twin when the chain is on.
    QObject::connect(vc.zoom_x, &QDoubleSpinBox::valueChanged, &mw, [&mw, &vc](double v) {
        if (vc.updating || !vc.chain) return;
        if (vc.chain->isChecked()) {
            vc.updating = true;
            vc.zoom_y->setValue(v);
            vc.updating = false;
        }
    });
    QObject::connect(vc.zoom_y, &QDoubleSpinBox::valueChanged, &mw, [&mw, &vc](double v) {
        if (vc.updating || !vc.chain) return;
        if (vc.chain->isChecked()) {
            vc.updating = true;
            vc.zoom_x->setValue(v);
            vc.updating = false;
        }
    });
    for (QDoubleSpinBox* spin : {vc.zoom_x, vc.zoom_y, vc.pos_x, vc.pos_y,
                                 vc.rotation, vc.anchor_x, vc.anchor_y}) {
        QObject::connect(spin, &QDoubleSpinBox::editingFinished, &mw, [&mw]() {
            apply_inspector_visual(mw, VisualPartTransform);
        });
    }
    for (QToolButton* btn : {vc.flip_h, vc.flip_v}) {
        QObject::connect(btn, &QToolButton::toggled, &mw, [&mw]() {
            apply_inspector_visual(mw, VisualPartTransform);
        });
    }
    QObject::connect(vc.blend, &QComboBox::currentIndexChanged, &mw, [&mw]() {
        apply_inspector_visual(mw, VisualPartComposite);
    });
    QObject::connect(vc.opacity_slider, &QSlider::valueChanged, &mw,
                     [&vc](int v) {
                         if (vc.opacity_value)
                             vc.opacity_value->setText(QString::number(v / 100.0, 'f', 2));
                     });
    QObject::connect(vc.opacity_slider, &QSlider::sliderReleased, &mw, [&mw]() {
        apply_inspector_visual(mw, VisualPartComposite);
    });
}

void attach_inspector_visual(MainWindow& mw, TimelineWidget* timeline) {
    VisualControls* vc = lookup(mw);
    if (!vc || !vc->zoom_x || !timeline || vc->attached) return;
    vc->attached = true;
    QObject::connect(timeline, &TimelineWidget::clip_selected, &mw,
                     [&mw](const canvas::core::Clip*) { update_inspector_visual(mw); });
    QObject::connect(timeline, &TimelineWidget::clips_range_selected, &mw,
                     [&mw](std::vector<canvas::core::ClipId>) { update_inspector_visual(mw); });
}

void update_inspector_visual(MainWindow& mw) {
    VisualControls* vc = lookup(mw);
    if (!vc || !vc->zoom_x || !mw.project_) return;
    canvas::core::Track::Kind kind;
    std::size_t index = 0;
    canvas::core::Clip clip;
    if (!mw.find_selected_clip(kind, index, clip)) return;

    vc->updating = true;
    vc->zoom_x->setValue(clip.scale_x);
    vc->zoom_y->setValue(clip.scale_y);
    vc->pos_x->setValue(clip.pos_x);
    vc->pos_y->setValue(clip.pos_y);
    vc->rotation->setValue(clip.rotation_deg);
    vc->anchor_x->setValue(clip.anchor_dx);
    vc->anchor_y->setValue(clip.anchor_dy);
    vc->flip_h->setChecked(clip.flip_h);
    vc->flip_v->setChecked(clip.flip_v);
    vc->blend->setCurrentIndex(static_cast<int>(clip.blend_mode));
    vc->opacity_slider->setValue(std::lround(clip.opacity * 100.0));
    vc->opacity_value->setText(QString::number(clip.opacity, 'f', 2));
    vc->updating = false;
}

void apply_inspector_visual(MainWindow& mw, unsigned parts) {
    VisualControls* vc = lookup(mw);
    if (!vc) return;

    if (parts & VisualPartTransform) {
        if (!vc->zoom_x) return;
        canvas::core::Track::Kind kind;
        std::size_t index = 0;
        canvas::core::Clip clip;
        if (!mw.find_selected_clip(kind, index, clip)) return;

        const float sx = static_cast<float>(vc->zoom_x->value());
        const float sy = static_cast<float>(vc->zoom_y->value());
        const double px = vc->pos_x->value();
        const double py = vc->pos_y->value();
        const float rot = static_cast<float>(vc->rotation->value());
        const double ax = vc->anchor_x->value();
        const double ay = vc->anchor_y->value();
        const bool fh = vc->flip_h->isChecked();
        const bool fv = vc->flip_v->isChecked();
        const bool same = clip.scale_x == sx && clip.scale_y == sy && clip.pos_x == px &&
                          clip.pos_y == py && clip.rotation_deg == rot &&
                          clip.anchor_dx == ax && clip.anchor_dy == ay &&
                          clip.flip_h == fh && clip.flip_v == fv;
        if (!same) {
            auto cmd = canvas::core::set_clip_transform(
                mw.project_->sequence, kind, index, clip.id, sx, sy, px, py, rot, ax, ay, fh, fv);
            if (cmd) {
                mw.undo_.record(std::move(cmd));
                mw.has_unsaved_changes_ = true;
                mw.refresh_timeline();
                mw.push_snapshot();
                qWarning() << "[edit] CLIP-TRANSFORM kind="
                           << (kind == canvas::core::Track::Kind::Video ? "V" : "A")
                           << "track=" << index << "clip=" << clip.id
                           << "scale=" << sx << "x" << sy << "pos=" << px << "," << py
                           << "rot=" << rot << "flip=" << fh << "," << fv;
            }
        }
    }

    if (parts & VisualPartComposite) {
        if (!vc->opacity_slider) return;
        canvas::core::Track::Kind kind;
        std::size_t index = 0;
        canvas::core::Clip clip;
        if (!mw.find_selected_clip(kind, index, clip)) return;

        const float opacity = static_cast<float>(vc->opacity_slider->value()) / 100.0f;
        const auto blend = static_cast<canvas::core::BlendMode>(vc->blend->currentIndex());
        const bool same = clip.opacity == opacity && clip.blend_mode == blend;
        if (!same) {
            auto cmd = canvas::core::set_clip_composite(
                mw.project_->sequence, kind, index, clip.id, opacity, blend);
            if (cmd) {
                mw.undo_.record(std::move(cmd));
                mw.has_unsaved_changes_ = true;
                mw.refresh_timeline();
                mw.push_snapshot();
                qWarning() << "[edit] CLIP-COMPOSITE kind="
                           << (kind == canvas::core::Track::Kind::Video ? "V" : "A")
                           << "track=" << index << "clip=" << clip.id
                           << "opacity=" << opacity << "blend=" << static_cast<int>(blend);
            }
        }
    }
}

void apply_inspector_visual(MainWindow& mw) {
    apply_inspector_visual(mw, VisualPartAll);
}

}  // namespace canvas::gui