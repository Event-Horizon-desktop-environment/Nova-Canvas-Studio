#include "features/timeline/view_options_menu.hpp"

#include "features/timeline/timeline_view_options.hpp"
#include "UX/MainWindow.hpp"
#include "UX/theme.hpp"
#include "UX/theme_menu.hpp"

#include <QAction>
#include <QActionGroup>
#include <QMenu>
#include <QSlider>
#include <QSettings>
#include <QWidgetAction>
#include <QLabel>
#include <QVBoxLayout>

namespace canvas::gui {

namespace {

void apply_fixed_playhead(MainWindow& mw) {
    // "Fixed Playhead": keep the playhead visually anchored — the timeline
    // scrolls beneath it during playback instead of the needle traveling.
    mw.timeline()->set_follow_playhead(mw.view_options().fixed_playhead);
}

// One vertical slider row inside a submenu: label with the live value + a
// horizontal slider. Emits on valueChanged so the view livens as you drag.
QWidget* make_height_slider_row(MainWindow& mw, const QString& title,
                                double initial, bool video) {
    auto* container = new QWidget;
    auto* layout = new QVBoxLayout(container);
    layout->setContentsMargins(10, 6, 10, 8);
    layout->setSpacing(4);
    auto* label = new QLabel(QStringLiteral("%1 — %2").arg(
        title, QString::number(qRound(initial))), container);
    label->setStyleSheet(QStringLiteral("color: %1; background: transparent;")
                             .arg(tokens().ink.name()));
    layout->addWidget(label);
    auto* slider = new QSlider(Qt::Horizontal, container);
    slider->setRange(24, 200);
    slider->setValue(qRound(initial) /* clamped to range */);
    layout->addWidget(slider);
    // Live apply per tick; the value re-label keeps the number honest.
    QObject::connect(slider, &QSlider::valueChanged, container, [&mw, label, video](int v) {
        label->setText(QStringLiteral("%1 — %2").arg(
            label->text().section(QStringLiteral(" — "), 0, 0),
            QString::number(v)));
        if (video) {
            mw.view_options().video_track_height = v;
            mw.timeline()->set_all_video_heights(v);
        } else {
            mw.view_options().audio_track_height = v;
            mw.timeline()->set_all_audio_heights(v);
        }
    });
    return container;
}

QAction* toggle(MainWindow& mw, QMenu* menu, const QString& text, bool* field,
                bool rethumb) {
    auto* act = menu->addAction(text);
    act->setCheckable(true);
    act->setChecked(*field);
    QObject::connect(act, &QAction::toggled, &mw, [&mw, field, rethumb](bool on) {
        *field = on;
        apply_view_options(mw);
    });
    return act;
}

}  // namespace

void apply_view_options(MainWindow& mw) {
    if (!mw.timeline() || !mw.viewer()) return;  // pre-widget construction guard
    TimelineViewOptions& opts = mw.view_options();
    mw.timeline()->set_view_options(&opts);
    apply_fixed_playhead(mw);
    mw.timeline()->notify_view_options_changed();
    mw.viewer()->set_viewer_background(opts.viewer_background);
}

void attach_timeline_view_options_button(MainWindow& mw, QToolButton* button) {
    // Baseline: any "Set as Default View" snapshot becomes this session's
    // starting options. (apply_view_options runs lazily: first menu edit, or
    // ShellCenter's post-timeline construction call, applies it.)
    {
        QSettings settings;
        load_view_options(settings, mw.view_options());
    }

    auto* menu = make_rounded_menu(button);
    menu->setTitle(QObject::tr("Timeline View Options"));

    toggle(mw, menu, QObject::tr("Display Stacked Timelines"),
           &mw.view_options().show_stacked_timelines, /*rethumb=*/false);
    toggle(mw, menu, QObject::tr("Display Subtitle Tracks"),
           &mw.view_options().show_subtitle_tracks, /*rethumb=*/false);
    menu->addSeparator();

    toggle(mw, menu, QObject::tr("Display Audio Waveforms"),
           &mw.view_options().show_waveforms, /*rethumb=*/true);
    toggle(mw, menu, QObject::tr("Display Clip Names"),
           &mw.view_options().show_clip_names, /*rethumb=*/false);
    toggle(mw, menu, QObject::tr("Display Clip Durations"),
           &mw.view_options().show_clip_durations, /*rethumb=*/false);
    menu->addSeparator();

    // Thumbnail View — exclusive radio group (Off / Single Frame / Filmstrip).
    // A real QActionGroup so checking one unchecks the others (and the active
    // entry can't be toggled off into a stale model value).
    {
        auto* sub = menu->addMenu(QObject::tr("Thumbnail View"));
        auto* group = new QActionGroup(sub);
        group->setExclusive(true);
        struct Mode { const char* label; ThumbnailMode mode; };
        const Mode modes[] = {{"Off", ThumbnailMode::Off},
                              {"Single Frame", ThumbnailMode::SingleFrame},
                              {"Filmstrip", ThumbnailMode::Filmstrip}};
        for (const auto& m : modes) {
            auto* act = sub->addAction(QObject::tr(m.label));
            act->setCheckable(true);
            group->addAction(act);
            const ThumbnailMode mode = m.mode;
            act->setChecked(mw.view_options().thumbnails == mode);
            QObject::connect(act, &QAction::toggled, &mw, [&mw, mode](bool on) {
                if (!on) return;
                mw.view_options().thumbnails = mode;
                apply_view_options(mw);
            });
        }
    }

    // Viewer Background — exclusive radio group (Black / Checkerboard / White /
    // Gray). Solid modes recolor the monitor letterbox; Checkerboard tiles it.
    // Grouped so choosing a new background clears the previous one's check.
    {
        auto* sub = menu->addMenu(QObject::tr("Viewer Background"));
        auto* group = new QActionGroup(sub);
        group->setExclusive(true);
        struct Bg { const char* label; ViewerBackground bg; };
        const Bg bgs[] = {{"Black", ViewerBackground::Black},
                          {"Checkerboard", ViewerBackground::Checkerboard},
                          {"White", ViewerBackground::White},
                          {"Gray", ViewerBackground::Gray}};
        for (const auto& b : bgs) {
            auto* act = sub->addAction(QObject::tr(b.label));
            act->setCheckable(true);
            group->addAction(act);
            const ViewerBackground bg = b.bg;
            act->setChecked(mw.view_options().viewer_background == bg);
            QObject::connect(act, &QAction::toggled, &mw, [&mw, bg](bool on) {
                if (!on) return;
                mw.view_options().viewer_background = bg;
                apply_view_options(mw);
            });
        }
    }
    menu->addSeparator();

    toggle(mw, menu, QObject::tr("Display Non-Rectified Waveforms"),
           &mw.view_options().non_rectified_waveforms, /*rethumb=*/true);
    toggle(mw, menu, QObject::tr("Display Full Waveforms"),
           &mw.view_options().full_waveforms, /*rethumb=*/true);
    toggle(mw, menu, QObject::tr("Display Waveform Borders"),
           &mw.view_options().waveform_borders, /*rethumb=*/true);
    toggle(mw, menu, QObject::tr("Display Scaled Waveforms"),
           &mw.view_options().scaled_waveforms, /*rethumb=*/true);
    menu->addSeparator();

    // Fixed Playhead: a latching toggle (playhead anchored, timeline scrolls).
    toggle(mw, menu, QObject::tr("Fixed Playhead"),
           &mw.view_options().fixed_playhead, /*rethumb=*/false);
    menu->addSeparator();

    // Track Height sliders (global: every row of that kind).
    {
        auto* sub = menu->addMenu(QObject::tr("Track Height"));
        auto* video = new QWidgetAction(sub);
        video->setDefaultWidget(make_height_slider_row(
            mw, QObject::tr("Video Track Height"), mw.view_options().video_track_height, true));
        sub->addAction(video);
        auto* audio = new QWidgetAction(sub);
        audio->setDefaultWidget(make_height_slider_row(
            mw, QObject::tr("Audio Track Height"), mw.view_options().audio_track_height, false));
        sub->addAction(audio);
    }
    menu->addSeparator();

    // Collapse / Expand All: one click strips every video AND audio track down
    // to its header chevron (and restores them). A single undoable command, so
    // one Undo flips the whole stack back.
    {
        auto* act = menu->addAction(QObject::tr("Collapse All Tracks"));
        QObject::connect(act, &QAction::triggered, &mw, [&mw] {
            if (!mw.project_) return;
            auto cmd = canvas::core::set_all_tracks_collapsed(mw.project_->sequence, true);
            if (!cmd) return;
            mw.has_unsaved_changes_ = true;
            mw.undo_.record(std::move(cmd));
            mw.refresh_timeline();
            mw.push_snapshot();
        });
        act = menu->addAction(QObject::tr("Expand All Tracks"));
        QObject::connect(act, &QAction::triggered, &mw, [&mw] {
            if (!mw.project_) return;
            auto cmd = canvas::core::set_all_tracks_collapsed(mw.project_->sequence, false);
            if (!cmd) return;
            mw.has_unsaved_changes_ = true;
            mw.undo_.record(std::move(cmd));
            mw.refresh_timeline();
            mw.push_snapshot();
        });
    }
    menu->addSeparator();

    // "Set as Default View": persist the current combination to QSettings;
    // every later session starts from it (Resolve's default-view contract).
    {
        auto* act = menu->addAction(QObject::tr("Set as Default View"));
        QObject::connect(act, &QAction::triggered, &mw, [&mw] {
            QSettings settings;
            save_view_options(settings, mw.view_options());
        });
    }

    button->setPopupMode(QToolButton::InstantPopup);
    button->setMenu(menu);
}

}  // namespace canvas::gui