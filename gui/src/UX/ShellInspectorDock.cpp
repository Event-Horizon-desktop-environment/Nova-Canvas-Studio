#include "UX/MainWindow.hpp"
#include "ui_MainWindow.h"

#include "UX/InspectorAudio.hpp"
#include "UX/InspectorFile.hpp"
#include "UX/InspectorSubtitles.hpp"
#include "UX/InspectorTransition.hpp"
#include "UX/InspectorVisual.hpp"
#include "UX/empty_state.hpp"
#include "UX/theme.hpp"

#include <QAction>
#include <QButtonGroup>
#include <QDockWidget>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QScrollArea>
#include <QSize>
#include <QSizePolicy>
#include <QStackedWidget>
#include <QToolButton>
#include <QVBoxLayout>
#include <QWidget>

#include <vector>

namespace canvas::gui {

void build_inspector_dock(MainWindow& mw) {
    mw.inspector_dock_ = mw.ui->inspectorDock;
    mw.inspector_dock_->setObjectName(QStringLiteral("inspectorDock"));
    apply_theme_style(mw.inspector_dock_, &dock_glow_style);
    auto* inspector_title = new QWidget(mw.inspector_dock_);
    inspector_title->setObjectName(QStringLiteral("inspectorDockTitle"));
    apply_theme_style(inspector_title, [] {
        return QStringLiteral("QWidget#inspectorDockTitle { background: transparent;"
                              " border: none; }");
    });
    mw.inspector_dock_->setTitleBarWidget(inspector_title);
    mw.inspector_dock_->setMinimumWidth(320);
    mw.inspector_dock_->setFeatures(QDockWidget::DockWidgetMovable | QDockWidget::DockWidgetFloatable);

    auto* inspector_body = new QWidget(mw.inspector_dock_);
    inspector_body->setObjectName(QStringLiteral("dockGlassCard"));
    apply_theme_style(inspector_body, &dock_panel_style);
    auto* inspector_outer = new QVBoxLayout(inspector_body);
    inspector_outer->setContentsMargins(0, 0, 0, 0);
    inspector_outer->setSpacing(0);

    auto* mode_row = new QWidget(inspector_body);
    apply_theme_style(mode_row, &inspector_tab_track_style);
    auto* mode_row_layout = new QHBoxLayout(mode_row);
    mode_row_layout->setContentsMargins(4, 4, 4, 4);
    mode_row_layout->setSpacing(2);
    struct ModePill { const char* label; const char* icon_name; };
    const ModePill modes[] = {
        {"Video", "settings"}, {"Audio", "volume"}, {"Effects", "mode"},
        {"Transition", "transition"}, {"Image", "viewport"}, {"File", "film-strip"},
        {"Subtitles", "subtitle"},
    };
    auto* mode_group = new QButtonGroup(mode_row);
    mode_group->setExclusive(true);
    std::vector<QToolButton*> mode_buttons;
    for (const auto& m : modes) {
        auto* b = new QToolButton(mode_row);
        const bool is_video = qstrcmp(m.label, "Video") == 0;
        b->setText(MainWindow::tr(m.label));
        b->setIcon(icon(m.icon_name));
        b->setIconSize(QSize(14, 14));
        b->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
        b->setCheckable(true);
        b->setChecked(is_video);
        b->setAutoRaise(true);
        apply_theme_style(b, &inspector_tab_style);
        b->setToolTip(MainWindow::tr(m.label));
        b->setMinimumWidth(1);
        b->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        mode_group->addButton(b);
        mode_row_layout->addWidget(b);
        mode_buttons.push_back(b);
    }
    inspector_outer->addWidget(mode_row);

    auto* scroll = new QScrollArea(inspector_body);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);

    auto* stack = new QStackedWidget(scroll);
    const int video_tab_index = 0;
    const int audio_tab_index = 1;
    const int transition_tab_index = 3;

    auto* video_page = new QWidget(stack);
    auto* video_layout = new QVBoxLayout(video_page);
    video_layout->setContentsMargins(0, 8, 0, 8);
    video_layout->setSpacing(0);

    build_inspector_visual(mw, video_layout);
    video_layout->addStretch(1);
    stack->addWidget(video_page);

    auto* audio_page = new QWidget(stack);
    auto* audio_layout = new QVBoxLayout(audio_page);
    audio_layout->setContentsMargins(0, 8, 0, 8);
    audio_layout->setSpacing(0);
    build_inspector_audio(mw, audio_layout, mode_buttons[audio_tab_index]);
    audio_layout->addStretch(1);
    stack->addWidget(audio_page);

    auto* effects_page = new QWidget(stack);
    {
        auto* page_layout = new QVBoxLayout(effects_page);
        page_layout->setContentsMargins(8, 8, 8, 8);
        page_layout->setSpacing(0);
        page_layout->addWidget(build_empty_state(
            effects_page, "effects", MainWindow::tr("Effects"),
            MainWindow::tr("Apply effects to the selected clip — coming in a future update.")));
    }
    stack->addWidget(effects_page);

    auto* transition_page = new QWidget(stack);
    auto* transition_layout = new QVBoxLayout(transition_page);
    transition_layout->setContentsMargins(0, 8, 0, 8);
    transition_layout->setSpacing(0);
    build_inspector_transition(mw, transition_layout, mode_buttons[transition_tab_index]);
    stack->addWidget(transition_page);

    auto* image_page = new QWidget(stack);
    {
        auto* page_layout = new QVBoxLayout(image_page);
        page_layout->setContentsMargins(8, 8, 8, 8);
        page_layout->setSpacing(0);
        page_layout->addWidget(build_empty_state(
            image_page, "viewport", MainWindow::tr("Image"),
            MainWindow::tr("Image controls for the selected clip — coming in a future update.")));
    }
    stack->addWidget(image_page);

    auto* file_page = new QWidget(stack);
    auto* file_layout = new QVBoxLayout(file_page);
    file_layout->setContentsMargins(0, 8, 0, 8);
    file_layout->setSpacing(0);
    build_inspector_file(mw, file_layout);
    stack->addWidget(file_page);

    auto* subtitles_page = new QWidget(stack);
    auto* subtitles_layout = new QVBoxLayout(subtitles_page);
    subtitles_layout->setContentsMargins(0, 8, 0, 8);
    subtitles_layout->setSpacing(0);
    build_inspector_subtitles(mw, subtitles_layout);
    subtitles_layout->addStretch(1);
    stack->addWidget(subtitles_page);

    for (std::size_t i = 0; i < mode_buttons.size(); ++i) {
        const int idx = static_cast<int>(i);
        QObject::connect(mode_buttons[idx], &QToolButton::toggled, stack, [stack, idx](bool on) {
            if (on) stack->setCurrentIndex(idx);
        });
    }
    stack->setCurrentIndex(video_tab_index);

    scroll->setWidget(stack);
    inspector_outer->addWidget(scroll, 1);

    mw.inspector_dock_->setWidget(inspector_body);
    mw.inspector_dock_->hide();
    QObject::connect(mw.inspector_toggle_action_, &QAction::toggled, mw.inspector_dock_, &QDockWidget::setVisible);
}

}
