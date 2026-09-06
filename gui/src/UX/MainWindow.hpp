#pragma once

#include <QLabel>
#include <QMainWindow>
#include <QPushButton>
#include <QSlider>
#include <QStringList>
#include <QElapsedTimer>

#include <memory>

#include "canvas/core/media/video_decoder.hpp"
#include "canvas/core/project/project.hpp"
#include "canvas/core/timeline/edit_ops.hpp"
#include "canvas/core/export/render_queue.hpp"
#include "features/deliver/deliver_settings_panel.hpp"
#include "features/deliver/render_queue_panel.hpp"
#include "features/playback/sequence_controller.hpp"
#include "features/thumbnails/thumbnail_service.hpp"
#include "Widgets/timeline_widget.hpp"
#include "Widgets/viewer_gl.hpp"

class QDockWidget;
class QKeyEvent;
class QAction;
class QMenu;
class QToolButton;
class QDoubleSpinBox;
class QVBoxLayout;
class MediaPoolWidget;
class QTimer;
class QTreeWidget;
namespace Ui {
class MainWindow;
}

namespace canvas::gui {

// Menu construction lives in ShellMenus.cpp (splitplan refactor) rather than the
// wall-of-layout builder. Declared here and friended so it can touch the chrome
// members it populates without widening MainWindow's public API.
class MainWindow;
void build_app_menus(MainWindow& main_window);

// Top status bar + page-mode foundation bar + transport bar construction live
// in ShellTopBar.cpp (splitplan refactor). Declared here and friended so they
// can touch the chrome members they populate without widening the public API.
QWidget* build_top_bar(MainWindow& main_window);
void build_page_bar(MainWindow& main_window);
QWidget* build_transport_bar(MainWindow& main_window);

// Left (bin tree + media pool) and right (inspector) dock population live in
// ShellDocks.cpp (splitplan refactor). Declared here and friended so they can
// touch the chrome members they populate with private-API access.
void build_left_dock(MainWindow& main_window);
void build_inspector_dock(MainWindow& main_window);

// The Inspector's Transform/Composite property block lives in InspectorVisual.cpp
// (splitplan refactor). build_inspector_visual() fills the Video tab's categories;
// attach_inspector_visual() connects the timeline's selection signals after the
// timeline exists; update_inspector_visual()/apply_inspector_visual() read and
// commit the selected clip's transform/composite fields.
void build_inspector_visual(MainWindow& main_window, QVBoxLayout* video_layout);
void attach_inspector_visual(MainWindow& main_window, TimelineWidget* timeline);
void update_inspector_visual(MainWindow& main_window);
void apply_inspector_visual(MainWindow& main_window);

// The center workspace (viewer column + contextual/toolbar chrome), the
// timeline dock, and the Deliver page docks live in ShellCenter.cpp (splitplan
// refactor). Declared here and friended so it can touch the chrome members it
// populates with private-API access.
void build_center_workspace(MainWindow& main_window);

class MainWindow final : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override;

    void open_file(const QString& path);

    // Deliver-page integration: the page bar toggles between the Edit layout
    // (media pool left + inspector right) and the Deliver layout (settings left,
    // render queue right) while keeping the shared viewer + timeline visible.
    void enter_deliver_page();
    void enter_edit_page();
    void add_current_to_render_queue();
    void render_all_from_queue();
    void reflect_render_queue();

protected:
    void keyPressEvent(QKeyEvent* event) override;
    void closeEvent(QCloseEvent* event) override;

private slots:
    void on_import_media();
    void on_new_project();
    void on_open_project();
    void on_open_recent_file(QAction* action);
    void on_save_project();
    void on_save_project_as();
    void on_undo();
    void on_redo();
    void on_position_changed(int64_t frame_number);
    void on_playback_changed(bool playing);
    void on_fps_tick();

private:
    void build_ui();
    void connect_timeline();
    void update_time_label();
    void refresh_timeline();
    void update_fps_label();
    bool save_project_to(const QString& path);
    void push_snapshot(int64_t initial_frame = -1);
    void delete_selected_clip(bool ripple);
    void toggle_disable_selected_clip();
    void toggle_transition_on_selected();
    void toggle_bookmark_at_playhead();
    void ensure_tracks(std::size_t min_video, std::size_t min_audio);
    bool place_selected_media(canvas::core::Placement mode);
    bool place_media_at(canvas::core::MediaId media_id, int64_t frame, canvas::core::Placement mode);
    void refresh_media_pool();
    void new_untitled_project();
    int import_media_paths(const QStringList& paths);
    void refresh_bin_tree();
    void set_current_bin(const QString& bin_name);
    QString current_bin() const { return current_bin_; }
    void rebuild_recent_menu();
    void remember_recent_project(const QString& path);
    QStringList recent_projects() const;
    // Inspector: refreshes the Audio category's Volume/Pan spins from the
    // selected clip (no-op and keeps their values when nothing is selected),
    // and commits the current spin values to the selected clip as one undoable
    // edit (set_clip_audio), then refreshes the timeline.
    void update_inspector_audio();
    void apply_inspector_audio();
    // Locates the selected clip in the sequence; returns its kind/index.
    bool find_selected_clip(canvas::core::Track::Kind& out_kind, std::size_t& out_index,
                            canvas::core::Clip& out_clip) const;

    Ui::MainWindow* ui = nullptr;
    QMenu* open_recent_menu_ = nullptr;
    QAction* inspector_toggle_action_ = nullptr;
    QToolButton* inspector_top_btn_ = nullptr;
    SequenceController controller_;
    ThumbnailService thumbnails_;
    ViewerGL* viewer_ = nullptr;
    TimelineWidget* timeline_ = nullptr;
    QSlider* scrub_ = nullptr;
    QPushButton* play_button_ = nullptr;
    QLabel* time_label_ = nullptr;
    QStatusBar* status_ = nullptr;
    QLabel* fps_label_ = nullptr;
    QTimer* fps_timer_ = nullptr;
    QElapsedTimer fps_clock_;
    double nominal_fps_ = 0.0;
    int fps_frames_ = 0;
    MediaPoolWidget* media_pool_ = nullptr;
    QTreeWidget* bin_tree_ = nullptr;
    QDockWidget* media_dock_ = nullptr;
    QDockWidget* inspector_dock_ = nullptr;
    QString current_bin_;  // empty = Master bin

    // Deliver page.
    canvas::core::RenderQueue render_queue_;
    DeliverSettingsPanel* deliver_settings_ = nullptr;
    RenderQueuePanel* deliver_queue_panel_ = nullptr;
    QDockWidget* deliver_settings_dock_ = nullptr;
    QDockWidget* deliver_queue_dock_ = nullptr;
    bool deliver_active_ = false;

    friend void build_app_menus(MainWindow& main_window);
    friend QWidget* build_top_bar(MainWindow& main_window);
    friend void build_page_bar(MainWindow& main_window);
    friend QWidget* build_transport_bar(MainWindow& main_window);
    friend void build_left_dock(MainWindow& main_window);
    friend void build_inspector_dock(MainWindow& main_window);
    friend void build_inspector_visual(MainWindow& main_window, QVBoxLayout* video_layout);
    friend void attach_inspector_visual(MainWindow& main_window, TimelineWidget* timeline);
    friend void update_inspector_visual(MainWindow& main_window);
    friend void apply_inspector_visual(MainWindow& main_window, unsigned parts);
    friend void apply_inspector_visual(MainWindow& main_window);
    friend void build_center_workspace(MainWindow& main_window);

    std::unique_ptr<canvas::core::Project> project_;
    canvas::core::UndoStack undo_;
    QString project_path_;

    double fps_ = 30.0;
    int64_t total_frames_ = -1;
    int64_t current_frame_ = 0;
    bool has_unsaved_changes_ = false;
    canvas::core::ClipId selected_clip_ = 0;
    QDoubleSpinBox* inspector_audio_volume_ = nullptr;
    QDoubleSpinBox* inspector_audio_pan_ = nullptr;
};

}
