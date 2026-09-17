#include "UX/InspectorTransition.hpp"

#include <QButtonGroup>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QSlider>
#include <QSpinBox>
#include <QStackedWidget>
#include <QToolButton>

#include <cmath>
#include <functional>
#include <map>
#include <optional>
#include <vector>

#include "UX/InspectorShared.hpp"
#include "UX/MainWindow.hpp"
#include "Widgets/timeline_widget.hpp"
#include "canvas/core/timeline/edit_ops.hpp"
#include "canvas/core/timeline/model.hpp"

namespace canvas::gui {

namespace {

using canvas::core::Clip;
using canvas::core::ClipId;
using canvas::core::Track;

QString muted_label_style() {
    return QStringLiteral("color: %1; font-size: 11px;").arg(css(tokens().ink_muted));
}

QString trans_combo_style() {
    const ThemeTokens& t = tokens();
    return QStringLiteral(
               "QComboBox { background-color: %1; color: %2; border: 1px solid %3;"
               " border-radius: 8px; padding: 3px 8px; }"
               "QComboBox::drop-down { border: none; width: 18px; }"
               "QComboBox QAbstractItemView { background-color: %1; color: %2;"
               " selection-background-color: %4; border: 1px solid %3;"
               " border-radius: 8px; padding: 2px; }")
        .arg(css(t.surface_low), css(t.ink_muted), css(t.border_soft), css(t.accent));
}

QString pill_button_style() {
    const ThemeTokens& t = tokens();
    return QStringLiteral(
               "QToolButton { background-color: %1; color: %2; border: 1px solid %3;"
               " border-radius: 8px; padding: 6px 18px; font-weight: 500; }"
               "QToolButton:hover { color: %4; border-color: %5; }"
               "QToolButton:checked { background-color: %6; border-color: %6;"
               " color: %7; font-weight: 600; }")
        .arg(css(t.surface_low), css(t.ink_muted), css(t.border_soft), css(t.ink),
             css(t.border), css(t.accent), css(t.on_accent));
}

QString ratio_slider_style() {
    const ThemeTokens& t = tokens();
    return QStringLiteral(
               "QSlider::groove:horizontal { height: 4px; background: %1; border-radius: 2px; }"
               "QSlider::handle:horizontal { width: 10px; background: %2; margin: -4px 0;"
               " border-radius: 5px; }")
        .arg(css(t.border_soft), css(t.accent));
}

QString ghost_button_style() {
    const ThemeTokens& t = tokens();
    return QStringLiteral(
               "QToolButton { color: %1; border: 1px solid %2; border-radius: 8px;"
               " padding: 3px 8px; background: %3; }"
               "QToolButton:hover { color: %4; border-color: %5; }"
               "QToolButton:disabled { color: %6; }")
        .arg(css(t.ink_faint), css(t.border_soft), css(t.surface_low), css(t.ink_muted),
             css(t.border), css(t.ink_muted));
}

QString align_button_style() {
    const ThemeTokens& t = tokens();
    return QStringLiteral(
               "QToolButton { background: %1; color: %2; border: 1px solid %3;"
               " border-radius: 8px; min-width: 26px; padding: 3px 0; }"
               "QToolButton:hover { border-color: %4; color: %5; }"
               "QToolButton:checked { color: %6; border-color: %7;"
               " background: %8; }")
        .arg(css(t.surface_low), css(t.ink_muted), css(t.border_soft), css(t.border),
             css(t.ink_muted), css(t.on_accent), css(t.accent), css(t.surface_higher));
}

struct SideControls {
    InspectorCategory* video_cat = nullptr;
    InspectorCategory* audio_cat = nullptr;
    QComboBox* video_type = nullptr;
    QDoubleSpinBox* video_secs = nullptr;
    QSpinBox* video_frames = nullptr;
    QToolButton* align_left = nullptr;
    QToolButton* align_center = nullptr;
    QToolButton* align_right = nullptr;
    QComboBox* style = nullptr;
    QSlider* start_ratio = nullptr;
    QDoubleSpinBox* start_ratio_spin = nullptr;
    QSlider* end_ratio = nullptr;
    QDoubleSpinBox* end_ratio_spin = nullptr;
    QComboBox* ease = nullptr;
    QSlider* curve = nullptr;
    QDoubleSpinBox* curve_spin = nullptr;
    QToolButton* curve_reset = nullptr;
    QComboBox* audio_fade_out = nullptr;
    QComboBox* audio_fade_in = nullptr;
    QDoubleSpinBox* audio_secs = nullptr;
    QSpinBox* audio_frames = nullptr;
    bool in_edge = false;
};

struct Registry {
    QToolButton* start_btn = nullptr;
    QToolButton* end_btn = nullptr;
    QStackedWidget* stack = nullptr;
    QToolButton* mode_btn = nullptr;
    QStackedWidget* inspector_stack = nullptr;
    int transition_page_index = -1;
    SideControls start;
    SideControls end;
    bool updating = false;
    bool attached = false;
    std::map<bool, int64_t> default_durations_frames;
};

Registry& registry(MainWindow& mw) {
    static std::map<MainWindow*, Registry> s_reg;
    return s_reg[&mw];
}

struct Resolved {
    Track::Kind kind = Track::Kind::Video;
    std::size_t track = 0;
    const Clip* clip = nullptr;
};

std::optional<Resolved> resolve_clip(const canvas::core::Project& proj, ClipId id) {
    if (id == 0) return std::nullopt;
    const auto& seq = proj.sequence;
    const auto find_in = [&](const std::vector<Track>& tracks, Track::Kind kind)
        -> std::optional<Resolved> {
        for (std::size_t i = 0; i < tracks.size(); ++i) {
            if (const Clip* c = tracks[i].clip_with_id(id)) return Resolved{kind, i, c};
        }
        return std::nullopt;
    };
    if (auto r = find_in(seq.video_tracks, Track::Kind::Video)) return r;
    if (auto r = find_in(seq.audio_tracks, Track::Kind::Audio)) return r;
    return std::nullopt;
}

std::optional<Resolved> audio_target(const canvas::core::Project& proj, const Resolved& v) {
    if (v.kind == Track::Kind::Audio) return v;
    if (!v.clip->linked_id) return std::nullopt;
    if (auto r = resolve_clip(proj, v.clip->linked_id)) return r;
    return std::nullopt;
}

std::optional<Resolved> side_video_target(const canvas::core::Project& proj,
                                          const TimelineWidget& timeline, const SideControls& sc) {
    if (!sc.in_edge) return resolve_clip(proj, timeline.selected_transition_a());
    const ClipId b = timeline.selected_transition_b();
    return resolve_clip(proj, b != 0 ? b : timeline.selected_transition_a());
}

int video_index_for(canvas::core::TransitionType t) {
    switch (t) {
        case canvas::core::TransitionType::CrossDissolve: return 1;
        case canvas::core::TransitionType::DipToBlack: return 2;
        case canvas::core::TransitionType::FadeOut: return 3;
        case canvas::core::TransitionType::FadeIn: return 4;
        case canvas::core::TransitionType::WipeLeft: return 5;
        case canvas::core::TransitionType::WipeRight: return 6;
        case canvas::core::TransitionType::WipeUp: return 7;
        case canvas::core::TransitionType::WipeDown: return 8;
        default: return 0;
    }
}

canvas::core::TransitionType video_type_from_index(int idx) {
    switch (idx) {
        case 1: return canvas::core::TransitionType::CrossDissolve;
        case 2: return canvas::core::TransitionType::DipToBlack;
        case 3: return canvas::core::TransitionType::FadeOut;
        case 4: return canvas::core::TransitionType::FadeIn;
        case 5: return canvas::core::TransitionType::WipeLeft;
        case 6: return canvas::core::TransitionType::WipeRight;
        case 7: return canvas::core::TransitionType::WipeUp;
        case 8: return canvas::core::TransitionType::WipeDown;
        default: return canvas::core::TransitionType::None;
    }
}

int audio_index_for(canvas::core::TransitionType t) {
    switch (t) {
        case canvas::core::TransitionType::AudioFadeConstantGain: return 1;
        case canvas::core::TransitionType::AudioFadeConstantPower: return 2;
        case canvas::core::TransitionType::AudioFadeExponential: return 3;
        default: return 0;
    }
}

canvas::core::TransitionType audio_type_from_index(int idx) {
    switch (idx) {
        case 1: return canvas::core::TransitionType::AudioFadeConstantGain;
        case 2: return canvas::core::TransitionType::AudioFadeConstantPower;
        case 3: return canvas::core::TransitionType::AudioFadeExponential;
        default: return canvas::core::TransitionType::None;
    }
}

float ease_amount_from_index(int idx) {
    switch (idx) {
        case 1: return 1.0f / 3.0f;
        case 2: return 2.0f / 3.0f;
        case 3: return 1.0f;
        default: return 0.0f;
    }
}

int ease_index_for(float amount) {
    if (amount <= 0.16f) return 0;
    if (amount <= 0.5f) return 1;
    if (amount <= 0.83f) return 2;
    return 3;
}

void set_dark_combo(QComboBox* cb, const QStringList& items) {
    cb->addItems(items);
    apply_theme_style(cb, &trans_combo_style);
}

QToolButton* make_pill_button(const QString& text) {
    auto* b = new QToolButton;
    b->setText(text);
    b->setCheckable(true);
    apply_theme_style(b, &pill_button_style);
    return b;
}

QSlider* make_ratio_slider(QWidget* parent) {
    auto* s = new QSlider(Qt::Horizontal, parent);
    s->setRange(0, 100);
    s->setMinimumWidth(0);
    apply_theme_style(s, &ratio_slider_style);
    return s;
}

QToolButton* make_ghost_button(const QString& text) {
    auto* b = new QToolButton;
    b->setText(text);
    apply_theme_style(b, &ghost_button_style);
    return b;
}

std::unique_ptr<canvas::core::ICommand> make_transition_cmd(
    canvas::core::Sequence& seq, const Resolved& r, bool in_edge,
    canvas::core::TransitionType type, int64_t duration) {
    return in_edge
               ? canvas::core::set_clip_transition_in(seq, r.kind, r.track, r.clip->id, type, duration)
               : canvas::core::set_clip_transition(seq, r.kind, r.track, r.clip->id, type, duration);
}

std::unique_ptr<canvas::core::ICommand> make_curve_cmd(canvas::core::Sequence& seq,
                                                       const Resolved& r, bool in_edge, float ease,
                                                       float curve_value, int start_ratio,
                                                       int end_ratio) {
    return canvas::core::set_clip_transition_curve(seq, r.kind, r.track, r.clip->id, in_edge,
                                                   ease, curve_value, start_ratio, end_ratio);
}

void populate_side(const canvas::core::Project& proj, const TimelineWidget& timeline,
                   Registry& reg, SideControls& sc) {
    auto target = side_video_target(proj, timeline, sc);
    const bool active = target.has_value();

    sc.video_cat->setEnabled(active);
    sc.audio_cat->setEnabled(active);
    if (!active) return;

    reg.updating = true;
    const Clip& clip = *target->clip;
    const bool in_edge = sc.in_edge;
    const canvas::core::TransitionType type =
        in_edge ? clip.transition_in : clip.transition_out;
    const int64_t duration = in_edge ? clip.transition_in_duration : clip.transition_out_duration;

    sc.video_type->setCurrentIndex(video_index_for(type));
    const double fps = proj.sequence.fps;
    const double secs = fps > 0.0 ? static_cast<double>(duration) / fps : 0.0;
    sc.video_secs->setValue(secs);
    sc.video_frames->setValue(duration);

    const float curve =
        in_edge ? clip.transition_in_curve_value : clip.transition_out_curve_value;
    const int64_t start_ratio =
        in_edge ? clip.transition_in_start_ratio : clip.transition_out_start_ratio;
    const int64_t end_ratio =
        in_edge ? clip.transition_in_end_ratio : clip.transition_out_end_ratio;
    const float ease = in_edge ? clip.transition_in_ease : clip.transition_out_ease;
    const int curve_pct = static_cast<int>(std::lround(curve * 100.0));
    sc.start_ratio->setValue(start_ratio);
    sc.start_ratio_spin->setValue(start_ratio);
    sc.end_ratio->setValue(end_ratio);
    sc.end_ratio_spin->setValue(end_ratio);
    sc.curve->setValue(curve_pct);
    sc.curve_spin->setValue(curve);
    sc.ease->setCurrentIndex(ease_index_for(ease));

    const auto audio = audio_target(proj, *target);
    if (!audio) {
        sc.audio_cat->setEnabled(false);
        reg.updating = false;
        return;
    }
    const Clip& aclip = *audio->clip;
    const canvas::core::TransitionType atype =
        in_edge ? aclip.transition_in : aclip.transition_out;
    const int64_t aduration = in_edge ? aclip.transition_in_duration
                                      : aclip.transition_out_duration;
    sc.audio_fade_out->setCurrentIndex(
        in_edge ? audio_index_for(aclip.transition_out) : audio_index_for(atype));
    sc.audio_fade_in->setCurrentIndex(
        in_edge ? audio_index_for(atype) : audio_index_for(aclip.transition_in));
    sc.audio_secs->setValue(fps > 0.0 ? static_cast<double>(aduration) / fps : 0.0);
    sc.audio_frames->setValue(aduration);
    sc.audio_cat->setEnabled(true);
    reg.updating = false;
}

}

void build_inspector_transition(MainWindow& mw, QVBoxLayout* transition_layout,
                                QToolButton* transition_mode_btn) {
    Registry& reg = registry(mw);
    reg.mode_btn = transition_mode_btn;
    if (auto* page_widget = transition_layout->parentWidget()) {
        if (auto* page_stack = qobject_cast<QStackedWidget*>(page_widget->parentWidget())) {
            reg.inspector_stack = page_stack;
            reg.transition_page_index = page_stack->indexOf(page_widget);
        }
    }
    auto* host = transition_layout->parentWidget();
    const auto tr = [](const char* s) { return MainWindow::tr(s); };

    const auto commit = [&mw](std::unique_ptr<canvas::core::ICommand>&& cmd) {
        if (!cmd) return;
        mw.undo_.record(std::move(cmd));
        mw.has_unsaved_changes_ = true;
        mw.push_snapshot();
        mw.refresh_timeline();
    };

    auto* pill_row = new QWidget(host);
    auto* pill_layout = new QHBoxLayout(pill_row);
    pill_layout->setContentsMargins(6, 8, 6, 0);
    pill_layout->setSpacing(6);
    reg.start_btn = make_pill_button(tr("Start"));
    reg.end_btn = make_pill_button(tr("End"));
    reg.start_btn->setChecked(true);
    pill_layout->addWidget(reg.start_btn);
    pill_layout->addWidget(reg.end_btn);
    pill_layout->addStretch(1);
    transition_layout->addWidget(pill_row);

    reg.stack = new QStackedWidget(host);
    transition_layout->addWidget(reg.stack, 1);

    const auto build_side = [&](bool in_edge) {
        SideControls& sc = in_edge ? reg.end : reg.start;
        sc.in_edge = in_edge;

        auto* page = new QWidget(reg.stack);
        auto* layout = new QVBoxLayout(page);
        layout->setContentsMargins(0, 8, 0, 0);
        layout->setSpacing(10);

        sc.video_cat = new InspectorCategory(tr("Video"), true, true);
        sc.audio_cat = new InspectorCategory(tr("Audio"), false, true);
        layout->addWidget(sc.video_cat);
        layout->addWidget(sc.audio_cat);

        auto* vbody = sc.video_cat->body_layout();
        sc.video_type = new QComboBox;
        set_dark_combo(sc.video_type,
                       {tr("None"), tr("Cross Dissolve"), tr("Dip To Black"), tr("Video Fade Out"),
                        tr("Video Fade In"), tr("Wipe Left"), tr("Wipe Right"), tr("Wipe Up"),
                        tr("Wipe Down")});
        add_property_row(vbody, tr("Transition Type"), sc.video_type);

        auto* dur_lbl = new QLabel(tr("Duration"));
        dur_lbl->setMinimumWidth(78);
        apply_theme_style(dur_lbl, &muted_label_style);
        auto* dur_row = new QHBoxLayout;
        dur_row->setSpacing(6);
        sc.video_secs = new QDoubleSpinBox;
        sc.video_secs->setRange(0.0, 600.0);
        sc.video_secs->setDecimals(2);
        sc.video_secs->setSuffix(tr(" s"));
        sc.video_secs->setKeyboardTracking(false);
        sc.video_frames = new QSpinBox;
        sc.video_frames->setRange(0, 1000000);
        sc.video_frames->setSuffix(tr(" f"));
        sc.video_frames->setKeyboardTracking(false);
        dur_row->addWidget(sc.video_secs, 1);
        dur_row->addWidget(sc.video_frames, 1);
        auto* dur_outer = new QHBoxLayout;
        dur_outer->setSpacing(6);
        dur_outer->addWidget(dur_lbl);
        dur_outer->addLayout(dur_row);
        vbody->addLayout(dur_outer);

        auto* default_btn = make_ghost_button(tr("Set as Default Duration"));
        vbody->addWidget(default_btn);

        auto* align_group = new QButtonGroup(page);
        align_group->setExclusive(true);
        const auto make_align = [&](const QString& glyph, const QString& tip) {
            auto* b = new QToolButton;
            b->setText(glyph);
            b->setCheckable(true);
            b->setToolTip(tip);
            apply_theme_style(b, &align_button_style);
            return b;
        };
        sc.align_left = make_align(QStringLiteral("\u25C4"), tr("Align Left"));
        sc.align_center = make_align(QStringLiteral("\u2022"), tr("Align Center"));
        sc.align_right = make_align(QStringLiteral("\u25BA"), tr("Align Right"));
        align_group->addButton(sc.align_left);
        align_group->addButton(sc.align_center);
        align_group->addButton(sc.align_right);
        (in_edge ? sc.align_center : sc.align_right)->setChecked(true);
        auto* align_row = new QHBoxLayout;
        align_row->setSpacing(4);
        align_row->addWidget(sc.align_left);
        align_row->addWidget(sc.align_center);
        align_row->addWidget(sc.align_right);
        align_row->addStretch(1);
        auto* align_lbl = new QLabel(tr("Alignment"));
        align_lbl->setMinimumWidth(78);
        apply_theme_style(align_lbl, &muted_label_style);
        auto* align_outer = new QHBoxLayout;
        align_outer->setSpacing(6);
        align_outer->addWidget(align_lbl);
        align_outer->addLayout(align_row);
        vbody->addLayout(align_outer);

        sc.style = new QComboBox;
        set_dark_combo(sc.style,
                       {tr("Standard"), tr("Soft"), tr("Smooth"), tr("Sleek"), tr("Glossy")});
        add_property_row(vbody, tr("Style"), sc.style);

        const auto make_ratio_row = [&](SideControls& side, const char* title, QSlider* slider,
                                        QDoubleSpinBox* spin) {
            slider->setRange(0, 100);
            spin->setRange(0.0, 100.0);
            spin->setDecimals(0);
            spin->setSuffix(tr("%"));
            spin->setKeyboardTracking(false);
            auto* ratio_row = new QHBoxLayout;
            ratio_row->setSpacing(6);
            ratio_row->addWidget(slider, 1);
            ratio_row->addWidget(spin);
            auto* lbl = new QLabel(MainWindow::tr(title));
            lbl->setMinimumWidth(78);
apply_theme_style(lbl, &muted_label_style);
            auto* outer = new QHBoxLayout;
            outer->setSpacing(6);
            outer->addWidget(lbl);
            outer->addLayout(ratio_row);
            side.video_cat->body_layout()->addLayout(outer);
        };
        sc.start_ratio = make_ratio_slider(page);
        sc.start_ratio_spin = new QDoubleSpinBox;
        make_ratio_row(sc, "Start Ratio", sc.start_ratio, sc.start_ratio_spin);
        sc.end_ratio = make_ratio_slider(page);
        sc.end_ratio_spin = new QDoubleSpinBox;
        make_ratio_row(sc, "End Ratio", sc.end_ratio, sc.end_ratio_spin);

        sc.ease = new QComboBox;
        set_dark_combo(sc.ease, {tr("None"), tr("Ease In"), tr("Ease Out"), tr("Ease In-Out")});
        add_property_row(vbody, tr("Ease"), sc.ease);

        sc.curve = make_ratio_slider(page);
        sc.curve->setRange(0, 100);
        sc.curve_spin = new QDoubleSpinBox;
        sc.curve_spin->setRange(0.0, 1.0);
        sc.curve_spin->setDecimals(2);
        sc.curve_spin->setSingleStep(0.05);
        sc.curve_spin->setKeyboardTracking(false);
        auto* curve_row = new QHBoxLayout;
        curve_row->setSpacing(6);
        curve_row->addWidget(sc.curve, 1);
        curve_row->addWidget(sc.curve_spin);
        auto* curve_lbl = new QLabel(tr("Transition Curve"));
        curve_lbl->setMinimumWidth(78);
        apply_theme_style(curve_lbl, &muted_label_style);
        auto* curve_outer = new QHBoxLayout;
        curve_outer->setSpacing(6);
        curve_outer->addWidget(curve_lbl);
        curve_outer->addLayout(curve_row);
        vbody->addLayout(curve_outer);

        auto* curve_tools = new QHBoxLayout;
        curve_tools->setSpacing(4);
        auto* kf_back = new QToolButton;
        kf_back->setIcon(icon("step_back"));
        kf_back->setIconSize(QSize(14, 14));
        kf_back->setAutoRaise(true);
        kf_back->setToolTip(tr("Previous keyframe (not available)"));
        kf_back->setEnabled(false);
        auto* kf_fwd = new QToolButton;
        kf_fwd->setIcon(icon("step_forward"));
        kf_fwd->setIconSize(QSize(14, 14));
        kf_fwd->setAutoRaise(true);
        kf_fwd->setToolTip(tr("Next keyframe (not available)"));
        kf_fwd->setEnabled(false);
        sc.curve_reset = make_ghost_button(tr("Reset"));
        curve_tools->addWidget(kf_back);
        curve_tools->addWidget(kf_fwd);
        curve_tools->addStretch(1);
        curve_tools->addWidget(sc.curve_reset);
        vbody->addLayout(curve_tools);

        auto* abody = sc.audio_cat->body_layout();
        sc.audio_fade_out = new QComboBox;
        set_dark_combo(sc.audio_fade_out,
                       {tr("None"), tr("Constant Gain"), tr("Constant Power"), tr("Exponential")});
        add_property_row(abody, tr("Fade Out"), sc.audio_fade_out);
        sc.audio_fade_in = new QComboBox;
        set_dark_combo(sc.audio_fade_in,
                       {tr("None"), tr("Constant Gain"), tr("Constant Power"), tr("Exponential")});
        add_property_row(abody, tr("Fade In"), sc.audio_fade_in);

        auto* adur_lbl = new QLabel(tr("Duration"));
        adur_lbl->setMinimumWidth(78);
        apply_theme_style(adur_lbl, &muted_label_style);
        auto* adur_row = new QHBoxLayout;
        adur_row->setSpacing(6);
        sc.audio_secs = new QDoubleSpinBox;
        sc.audio_secs->setRange(0.0, 600.0);
        sc.audio_secs->setDecimals(2);
        sc.audio_secs->setSuffix(tr(" s"));
        sc.audio_secs->setKeyboardTracking(false);
        sc.audio_frames = new QSpinBox;
        sc.audio_frames->setRange(0, 1000000);
        sc.audio_frames->setSuffix(tr(" f"));
        sc.audio_frames->setKeyboardTracking(false);
        adur_row->addWidget(sc.audio_secs, 1);
        adur_row->addWidget(sc.audio_frames, 1);
        auto* adur_outer = new QHBoxLayout;
        adur_outer->setSpacing(6);
        adur_outer->addWidget(adur_lbl);
        adur_outer->addLayout(adur_row);
        abody->addLayout(adur_outer);

        const auto commit_edits = [&mw, &reg](const std::function<void()>& body) {
            if (reg.updating) return;
            body();
        };

        QObject::connect(sc.video_type, qOverload<int>(&QComboBox::currentIndexChanged), &mw,
                         [&mw, &reg, &sc, commit, commit_edits]() {
            commit_edits([&]() {
                auto r = side_video_target(*mw.project_, *mw.timeline_, sc);
                if (!r) return;
                commit(make_transition_cmd(mw.project_->sequence, *r, sc.in_edge,
                                           video_type_from_index(sc.video_type->currentIndex()),
                                           sc.video_frames->value()));
            });
        });

        const auto sync_duration_spins = [&sc](double fps) {
            const int64_t frames = sc.video_frames->value();
            sc.video_secs->setValue(fps > 0.0 ? static_cast<double>(frames) / fps : 0.0);
        };
        QObject::connect(sc.video_frames, &QSpinBox::editingFinished, &mw,
                         [&mw, &reg, &sc, commit, commit_edits, sync_duration_spins]() {
            commit_edits([&]() {
                sync_duration_spins(mw.project_->sequence.fps);
                auto r = side_video_target(*mw.project_, *mw.timeline_, sc);
                if (!r) return;
                commit(make_transition_cmd(mw.project_->sequence, *r, sc.in_edge,
                                           video_type_from_index(sc.video_type->currentIndex()),
                                           sc.video_frames->value()));
            });
        });
        QObject::connect(sc.video_secs, &QDoubleSpinBox::editingFinished, &mw,
                         [&mw, &reg, &sc, commit, commit_edits, sync_duration_spins]() {
            commit_edits([&]() {
                const double fps = mw.project_->sequence.fps;
                sc.video_frames->setValue(static_cast<int64_t>(std::lround(sc.video_secs->value() * fps)));
                sync_duration_spins(fps);
                auto r = side_video_target(*mw.project_, *mw.timeline_, sc);
                if (!r) return;
                commit(make_transition_cmd(mw.project_->sequence, *r, sc.in_edge,
                                           video_type_from_index(sc.video_type->currentIndex()),
                                           sc.video_frames->value()));
            });
        });

        QObject::connect(default_btn, &QToolButton::clicked, &mw, [default_btn, &reg, &sc]() {
            reg.default_durations_frames[sc.in_edge] = sc.video_frames->value();
            default_btn->setToolTip(MainWindow::tr("Default duration set: %1 frames")
                                        .arg(sc.video_frames->value()));
        });

        const auto commit_shaping_edits = [&mw, &reg, &sc, commit]() {
            if (reg.updating) return;
            auto r = side_video_target(*mw.project_, *mw.timeline_, sc);
            if (!r) return;
            commit(make_curve_cmd(mw.project_->sequence, *r, sc.in_edge,
                                  ease_amount_from_index(sc.ease->currentIndex()),
                                  static_cast<float>(sc.curve->value()) / 100.0f,
                                  sc.start_ratio->value(), sc.end_ratio->value()));
        };
        const auto push_shaping_ui = [&sc]() {
            sc.start_ratio_spin->setValue(sc.start_ratio->value());
            sc.end_ratio_spin->setValue(sc.end_ratio->value());
            sc.curve_spin->setValue(static_cast<double>(sc.curve->value()) / 100.0);
        };
        const auto wire_ratio = [&](QSlider* slider, QDoubleSpinBox* spin) {
            QObject::connect(slider, &QSlider::sliderReleased, &mw,
                             [&sc, slider, spin, push_shaping_ui, commit_shaping_edits]() {
                                 spin->setValue(slider->value());
                                 push_shaping_ui();
                                 commit_shaping_edits();
                             });
            QObject::connect(spin, &QDoubleSpinBox::editingFinished, &mw,
                             [&sc, slider, spin, push_shaping_ui, commit_shaping_edits]() {
                                 slider->setValue(static_cast<int>(std::lround(spin->value())));
                                 push_shaping_ui();
                                 commit_shaping_edits();
                             });
        };
        wire_ratio(sc.start_ratio, sc.start_ratio_spin);
        wire_ratio(sc.end_ratio, sc.end_ratio_spin);
        QObject::connect(sc.curve, &QSlider::sliderReleased, &mw,
                         [&sc, push_shaping_ui, commit_shaping_edits]() {
                             push_shaping_ui();
                             commit_shaping_edits();
                         });
        QObject::connect(sc.curve_spin, &QDoubleSpinBox::editingFinished, &mw,
                         [&sc, push_shaping_ui, commit_shaping_edits]() {
                             sc.curve->setValue(static_cast<int>(std::lround(sc.curve_spin->value() * 100.0)));
                             push_shaping_ui();
                             commit_shaping_edits();
                         });
        QObject::connect(sc.ease, qOverload<int>(&QComboBox::currentIndexChanged), &mw,
                         [commit_shaping_edits]() { commit_shaping_edits(); });
        const auto apply_style = [&sc, &reg, push_shaping_ui, commit_shaping_edits]() {
            if (reg.updating) return;
            int curve_pct = sc.curve->value();
            int ease_idx = sc.ease->currentIndex();
            switch (sc.style->currentIndex()) {
                case 1: ease_idx = 2; curve_pct = 40; break;
                case 2: ease_idx = 3; curve_pct = 50; break;
                case 3: ease_idx = 1; curve_pct = 35; break;
                case 4: ease_idx = 2; curve_pct = 60; break;
                default: break;
            }
            reg.updating = true;
            sc.ease->setCurrentIndex(ease_idx);
            sc.curve->setValue(curve_pct);
            push_shaping_ui();
            reg.updating = false;
            commit_shaping_edits();
        };
        QObject::connect(sc.style, qOverload<int>(&QComboBox::currentIndexChanged), &mw, apply_style);

        QObject::connect(sc.curve_reset, &QToolButton::clicked, &mw,
                         [&sc, &reg, push_shaping_ui, commit_shaping_edits]() {
            reg.updating = true;
            if (sc.style->currentIndex() != 0) sc.style->setCurrentIndex(0);
            if (sc.ease->currentIndex() != 0) sc.ease->setCurrentIndex(0);
            sc.curve->setValue(sc.in_edge ? 0 : 100);
            sc.start_ratio->setValue(0);
            sc.end_ratio->setValue(100);
            push_shaping_ui();
            reg.updating = false;
            commit_shaping_edits();
        });

        const auto sync_audio_duration = [&sc](double fps) {
            const int64_t frames = sc.audio_frames->value();
            sc.audio_secs->setValue(fps > 0.0 ? static_cast<double>(frames) / fps : 0.0);
        };
        QComboBox* const owned_fade = in_edge ? sc.audio_fade_in : sc.audio_fade_out;
        QComboBox* const preview_fade = in_edge ? sc.audio_fade_out : sc.audio_fade_in;
        preview_fade->setEnabled(false);

        const auto commit_audio_edits = [&mw, &reg, &sc, owned_fade, commit]() {
            if (reg.updating) return;
            auto r = side_video_target(*mw.project_, *mw.timeline_, sc);
            if (!r) return;
            auto audio = audio_target(*mw.project_, *r);
            if (!audio) return;
            commit(make_transition_cmd(mw.project_->sequence, *audio, sc.in_edge,
                                       audio_type_from_index(owned_fade->currentIndex()),
                                       sc.audio_frames->value()));
        };
        QObject::connect(owned_fade, qOverload<int>(&QComboBox::currentIndexChanged), &mw,
                         [commit_audio_edits]() { commit_audio_edits(); });
        QObject::connect(sc.audio_frames, &QSpinBox::editingFinished, &mw,
                         [&mw, &sc, sync_audio_duration, commit_audio_edits]() {
            sync_audio_duration(mw.project_->sequence.fps);
            commit_audio_edits();
        });
        QObject::connect(sc.audio_secs, &QDoubleSpinBox::editingFinished, &mw,
                         [&mw, &sc, sync_audio_duration, commit_audio_edits]() {
            sc.audio_frames->setValue(
                static_cast<int64_t>(std::lround(sc.audio_secs->value() * mw.project_->sequence.fps)));
            sync_audio_duration(mw.project_->sequence.fps);
            commit_audio_edits();
        });

        layout->addStretch(1);
        return page;
    };

    reg.stack->addWidget(build_side(false));
    reg.stack->addWidget(build_side(true));

    QObject::connect(reg.start_btn, &QToolButton::toggled, &mw, [&reg](bool on) {
        if (on && reg.stack) reg.stack->setCurrentIndex(0);
    });
    QObject::connect(reg.end_btn, &QToolButton::toggled, &mw, [&reg](bool on) {
        if (on && reg.stack) reg.stack->setCurrentIndex(1);
    });
}

void attach_inspector_transition(MainWindow& mw, TimelineWidget* timeline) {
    Registry& reg = registry(mw);
    if (!timeline || !reg.stack || reg.attached) return;
    reg.attached = true;
    QObject::connect(timeline, &TimelineWidget::transition_selected, &mw,
                     [&mw](canvas::core::ClipId, canvas::core::ClipId, bool) {
                         update_inspector_transition(mw);
                     });
    QObject::connect(timeline, &TimelineWidget::transition_selection_cleared, &mw,
                     [&mw]() { update_inspector_transition(mw); });
    update_inspector_transition(mw);
}

void update_inspector_transition(MainWindow& mw) {
    Registry& reg = registry(mw);
    if (!reg.stack) return;

    const bool active = mw.timeline_ && mw.timeline_->has_selected_transition();
    reg.stack->setEnabled(active);
    if (reg.start_btn) reg.start_btn->setEnabled(active);
    if (reg.end_btn) reg.end_btn->setEnabled(active);
    if (reg.mode_btn) reg.mode_btn->setEnabled(active);
    if (!active) {
        if (reg.inspector_stack && reg.inspector_stack->currentIndex() == reg.transition_page_index)
            reg.inspector_stack->setCurrentIndex(0);
        return;
    }
    if (!mw.project_ || !mw.timeline_) return;

    if (reg.stack->currentIndex() == 0)
        populate_side(*mw.project_, *mw.timeline_, reg, reg.start);
    else
        populate_side(*mw.project_, *mw.timeline_, reg, reg.end);
    populate_side(*mw.project_, *mw.timeline_, reg,
                  reg.stack->currentIndex() == 0 ? reg.end : reg.start);
}

void apply_inspector_transition(MainWindow& mw) {
    (void)mw;
}

}