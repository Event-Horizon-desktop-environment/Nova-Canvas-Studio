#pragma once

#include <QLabel>
#include <QMainWindow>
#include <QToolButton>
#include <QSlider>
#include <QStringList>
#include <QElapsedTimer>

#include <memory>
#include <optional>
#include <vector>

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

// Color-page chrome module (features/color/color_page.cpp) is a new-file
// builder; the strip type is only pointer-held here, so a forward decl suffices.
class MiniTimelineStrip;

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

// Audio, Transition, and File inspector pages (InspectorAudio.cpp /
// InspectorTransition.cpp / InspectorFile.cpp, splitplan refactor). Friended so
// the page builders can drive the chrome members and read the selection state.
void build_inspector_audio(MainWindow& main_window, QVBoxLayout* audio_layout,
                           QToolButton* audio_mode_button);
void attach_inspector_audio(MainWindow& main_window, TimelineWidget* timeline);
void update_inspector_audio_full(MainWindow& main_window);
void apply_inspector_audio_processing(MainWindow& main_window);
void build_inspector_transition(MainWindow& main_window, QVBoxLayout* transition_layout,
                                QToolButton* transition_mode_btn);
void attach_inspector_transition(MainWindow& main_window, TimelineWidget* timeline);
void update_inspector_transition(MainWindow& main_window);
void apply_inspector_transition(MainWindow& main_window);
void build_inspector_file(MainWindow& main_window, QVBoxLayout* file_layout);
void attach_inspector_file(MainWindow& main_window, TimelineWidget* timeline);
void update_inspector_file(MainWindow& main_window);
void apply_inspector_file(MainWindow& main_window);

// The center workspace (viewer column + contextual/toolbar chrome), the
// timeline dock, and the Deliver page docks live in ShellCenter.cpp (splitplan
// refactor). Declared here and friended so it can touch the chrome members it
// populates with private-API access.
void build_center_workspace(MainWindow& main_window);

// The Color page workspace + panels live in features/color/ (splitplan-style
// builder, new module): build_color_page() creates the docks once the center
// workspace exists; enter/leave_color_page() are the page-bar handoffs.
// Declared here and friended so the module can own the Color chrome members.
void build_color_page(MainWindow& main_window);
void enter_color_page(MainWindow& main_window);
void leave_color_page(MainWindow& main_window);

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
    void resizeEvent(QResizeEvent* event) override;

private:
    // NOTE: no `slots` on these. setupUi() calls QMetaObject::connectSlotsByName,
    // which scans every moc-registered `on_*` slot against the .ui's designer
    // widget names and warns per-launch for the ones that never match. All of
    // them are wired via explicit function-pointer connects, so they're plain
    // member functions.
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
    // Live audio-mix snapshot: pushes the project to the playback worker WITHOUT
    // stopping playback or tearing down decoders, so volume/pan/pitch/EQ edits
    // land in the next mixed buffer as video keeps playing.
    void push_audio_mix_snapshot();
    void delete_selected_clip(bool ripple);
    void delete_selected_media();
    void delete_selected_media_and_clips();
    void toggle_disable_selected_clip();
    void toggle_transition_on_selected();
    void remove_all_transitions();
    void toggle_bookmark_at_playhead();
    // Grows the sequence's track list of the given kind until it covers
    // `index` (inclusive), naming new channels Vn/An by their 1-based order.
    void ensure_tracks_at(canvas::core::Track::Kind kind, std::size_t index);
    bool place_selected_media(canvas::core::Placement mode);
    bool place_media_at(canvas::core::MediaId media_id, int64_t frame, canvas::core::Placement mode,
                        std::optional<double> drop_scene_y = std::nullopt);
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
    // Live-only waveform feedback for the volume knob: re-renders the selected
    // clip's timeline spectrum at `vol_db` without committing an edit; the
    // actual volume command still lands from apply_inspector_audio() on release.
    void preview_inspector_volume(float vol_db);
    // Locates the selected clip in the sequence; returns its kind/index.
    bool find_selected_clip(canvas::core::Track::Kind& out_kind, std::size_t& out_index,
                            canvas::core::Clip& out_clip) const;
    // Locates the audio clip an audio edit should target: the selected audio
    // clip itself, or the linked audio mate of a selected video clip. False when
    // the selection has no audio to edit.
    bool find_audio_target(canvas::core::Track::Kind& out_kind, std::size_t& out_index,
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
    QToolButton* play_button_ = nullptr;
    QLabel* time_label_ = nullptr;
    QStatusBar* status_ = nullptr;
    QLabel* fps_label_ = nullptr;
    QTimer* fps_timer_ = nullptr;
    QElapsedTimer fps_clock_;
    double nominal_fps_ = 0.0;
    int fps_frames_ = 0;
    double render_fps_ = 0.0;  // > 0 while a render job is running; the top-bar fps label shows this
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

    // Color page (features/color/*, M0 UX scaffold).
    bool color_active_ = false;
    MiniTimelineStrip* color_mini_strip_ = nullptr;
    QDockWidget* color_dock_ = nullptr;
    QDockWidget* color_left_dock_ = nullptr;
    QDockWidget* color_nodes_dock_ = nullptr;
    QDockWidget* color_effects_dock_ = nullptr;
    QDockWidget* color_lightbox_dock_ = nullptr;

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
    friend void build_inspector_audio(MainWindow& main_window, QVBoxLayout* audio_layout,
                                      QToolButton* audio_mode_button);
    friend void attach_inspector_audio(MainWindow& main_window, TimelineWidget* timeline);
    friend void update_inspector_audio_full(MainWindow& main_window);
    friend void apply_inspector_audio_processing(MainWindow& main_window);
    friend void build_inspector_transition(MainWindow& main_window, QVBoxLayout* transition_layout,
                                           QToolButton* transition_mode_btn);
    friend void attach_inspector_transition(MainWindow& main_window, TimelineWidget* timeline);
    friend void update_inspector_transition(MainWindow& main_window);
    friend void apply_inspector_transition(MainWindow& main_window);
    friend void build_inspector_file(MainWindow& main_window, QVBoxLayout* file_layout);
    friend void attach_inspector_file(MainWindow& main_window, TimelineWidget* timeline);
    friend void update_inspector_file(MainWindow& main_window);
    friend void apply_inspector_file(MainWindow& main_window);
    friend void build_center_workspace(MainWindow& main_window);
    friend void build_color_page(MainWindow& main_window);
    friend void enter_color_page(MainWindow& main_window);
    friend void leave_color_page(MainWindow& main_window);

    std::unique_ptr<canvas::core::Project> project_;
    canvas::core::UndoStack undo_;
    QString project_path_;

    double fps_ = 30.0;
    int64_t total_frames_ = -1;
    int64_t current_frame_ = 0;
    bool has_unsaved_changes_ = false;
    canvas::core::ClipId selected_clip_ = 0;
    // The full visible selection (ids, incl. linked mates) from the timeline —
    // PRIMARY clip drives the Visual inspector, the whole set drives mixer
    // edits (Phase 4): Volume/Pan apply to every resolved audio target.
    std::vector<canvas::core::ClipId> selected_clip_ids_;
    QDoubleSpinBox* inspector_audio_volume_ = nullptr;
    QDoubleSpinBox* inspector_audio_pan_ = nullptr;
};

}
