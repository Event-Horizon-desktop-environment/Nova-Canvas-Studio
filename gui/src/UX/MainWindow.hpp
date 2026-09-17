#pragma once

#include <QLabel>
#include <QMainWindow>
#include <QPointer>
#include <QToolButton>
#include <QSlider>
#include <QStringList>
#include <QElapsedTimer>

#include <memory>
#include <optional>
#include <atomic>
#include <string>
#include <thread>
#include <vector>

#include "canvas/core/media/video_decoder.hpp"
#include "canvas/core/project/project.hpp"
#include "canvas/core/timeline/edit_ops.hpp"
#include "canvas/core/export/render_queue.hpp"
#include "canvas/core/media/transcribe.hpp"
#include "canvas/core/timeline/captions.hpp"
#include "features/deliver/deliver_settings_panel.hpp"
#include "features/deliver/render_queue_panel.hpp"
#include "features/project/project_manager_widget.hpp"
#include "features/playback/sequence_controller.hpp"
#include "features/source_preview/source_preview_controller.hpp"
#include "features/source_preview/source_viewer_panel.hpp"
#include "features/thumbnails/thumbnail_service.hpp"
#include "features/timeline/subtitle_dialog.hpp"
#include "features/timeline/timeline_view_options.hpp"
#include "Widgets/timeline_widget.hpp"
#include "Widgets/viewer_gl.hpp"

class QDockWidget;
class QKeyEvent;
class QAction;
class QMenu;
class QToolButton;
class QTimer;
class QDoubleSpinBox;
class QVBoxLayout;
class MediaPoolWidget;
class QTimer;
class QTreeWidget;
namespace Ui {
class MainWindow;
}

namespace canvas::gui {

inline constexpr std::uint64_t kPoolThumbNs = 0x8000000000000000ULL;

class MiniTimelineStrip;

class MainWindow;
void build_app_menus(MainWindow& main_window);

QWidget* build_top_bar(MainWindow& main_window);
void build_page_bar(MainWindow& main_window);
QWidget* build_transport_bar(MainWindow& main_window);

void build_left_dock(MainWindow& main_window);
void build_inspector_dock(MainWindow& main_window);

void build_inspector_visual(MainWindow& main_window, QVBoxLayout* video_layout);
void attach_inspector_visual(MainWindow& main_window, TimelineWidget* timeline);
void update_inspector_visual(MainWindow& main_window);
void apply_inspector_visual(MainWindow& main_window);

void build_inspector_audio(MainWindow& main_window, QVBoxLayout* audio_layout,
                           QToolButton* audio_mode_button);
void attach_inspector_audio(MainWindow& main_window, TimelineWidget* timeline);
void update_inspector_audio_full(MainWindow& main_window);
void apply_inspector_audio_processing(MainWindow& main_window);
void apply_inspector_voice_isolation(MainWindow& main_window);
void build_inspector_transition(MainWindow& main_window, QVBoxLayout* transition_layout,
                                QToolButton* transition_mode_btn);
void attach_inspector_transition(MainWindow& main_window, TimelineWidget* timeline);
void update_inspector_transition(MainWindow& main_window);
void apply_inspector_transition(MainWindow& main_window);
void build_inspector_file(MainWindow& main_window, QVBoxLayout* file_layout);
void attach_inspector_file(MainWindow& main_window, TimelineWidget* timeline);
void update_inspector_file(MainWindow& main_window);
void apply_inspector_file(MainWindow& main_window);

void build_inspector_subtitles(MainWindow& main_window, QVBoxLayout* subtitles_layout);
void attach_inspector_subtitles(MainWindow& main_window, TimelineWidget* timeline);
void update_inspector_subtitles(MainWindow& main_window);
void apply_inspector_subtitles(MainWindow& main_window);

void build_center_workspace(MainWindow& main_window);
void build_deliver_docks(MainWindow& main_window);

void build_color_page(MainWindow& main_window);
void enter_color_page(MainWindow& main_window);
void leave_color_page(MainWindow& main_window);

class MainWindow final : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override;

    void open_file(const QString& path);

    void enter_deliver_page();
    void enter_edit_page();

    void enter_project_manager();
    void leave_project_manager();

    void add_current_to_render_queue();
    void render_all_from_queue();
    void reflect_render_queue();

    void export_edl();

    void open_source_preview(const canvas::core::MediaEntry& media);
    void clear_source_preview();

    [[nodiscard]] TimelineViewOptions& view_options() { return view_options_; }

    [[nodiscard]] TimelineWidget* timeline() const { return timeline_; }
    [[nodiscard]] ViewerGL* viewer() const { return viewer_; }

    [[nodiscard]] bool subtitle_busy() const { return subtitle_busy_.load(); }
    void open_subtitle_dialog();

protected:
    void keyPressEvent(QKeyEvent* event) override;
    void closeEvent(QCloseEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;

private:
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
    void maybe_autosave();
    void push_audio_mix_snapshot();
    void push_grade_snapshot();
    void push_live_snapshot();
    void delete_selected_clip(bool ripple);
    void delete_selected_media();
    void delete_selected_media_and_clips();
    void toggle_disable_selected_clip();
    void toggle_transition_on_selected();
    void remove_all_transitions();
    void toggle_bookmark_at_playhead();
    void apply_clip_color(uint8_t color);
    void add_title_clip();
    void ensure_tracks_at(canvas::core::Track::Kind kind, std::size_t index);
    bool place_selected_media(canvas::core::Placement mode);
    bool place_media_at(canvas::core::MediaId media_id, int64_t frame, canvas::core::Placement mode,
                        std::optional<double> drop_scene_y = std::nullopt);
    void finish_subtitle_transcription(canvas::core::transcribe::Report report);
    void place_title_at(const QString& preset_id, int64_t frame);
    void apply_transition_from_toolbox(const QString& transition_id, int64_t frame,
                                       double scene_y);
    void refresh_media_pool();
    void new_untitled_project();
    int import_media_paths(const QStringList& paths);
    void refresh_bin_tree();
    void set_current_bin(const QString& bin_name);
    QString current_bin() const { return current_bin_; }
    void rebuild_recent_menu();
    void remember_recent_project(const QString& path);
    QStringList recent_projects() const;
    void update_inspector_audio();
    void apply_inspector_audio();
    void preview_inspector_volume(float vol_db);
    bool find_selected_clip(canvas::core::Track::Kind& out_kind, std::size_t& out_index,
                            canvas::core::Clip& out_clip) const;
    void activate_color_clip(canvas::core::ClipId id);
    bool find_audio_target(canvas::core::Track::Kind& out_kind, std::size_t& out_index,
                           canvas::core::Clip& out_clip) const;

    Ui::MainWindow* ui = nullptr;
    QMenu* open_recent_menu_ = nullptr;
    QAction* inspector_toggle_action_ = nullptr;
    QToolButton* inspector_top_btn_ = nullptr;
    SequenceController controller_;
    source_preview::SourcePreviewController src_preview_;
    ThumbnailService thumbnails_;
    ViewerGL* viewer_ = nullptr;
    source_preview::SourceViewerPanel* source_panel_ = nullptr;
    TimelineWidget* timeline_ = nullptr;
    TimelineViewOptions view_options_;
    QSlider* scrub_ = nullptr;
    QToolButton* play_button_ = nullptr;
    QLabel* time_label_ = nullptr;
    QStatusBar* status_ = nullptr;
    QLabel* fps_label_ = nullptr;
    QTimer* fps_timer_ = nullptr;
    QElapsedTimer fps_clock_;
    double nominal_fps_ = 0.0;
    int fps_frames_ = 0;
    double render_fps_ = 0.0;
    MediaPoolWidget* media_pool_ = nullptr;
    QTreeWidget* bin_tree_ = nullptr;
    QDockWidget* media_dock_ = nullptr;
    QDockWidget* inspector_dock_ = nullptr;
    QString current_bin_;

    canvas::core::RenderQueue render_queue_;
    DeliverSettingsPanel* deliver_settings_ = nullptr;
    RenderQueuePanel* deliver_queue_panel_ = nullptr;
    QDockWidget* deliver_settings_dock_ = nullptr;
    QDockWidget* deliver_queue_dock_ = nullptr;
    bool deliver_active_ = false;

    bool color_active_ = false;
    MiniTimelineStrip* color_mini_strip_ = nullptr;
    QDockWidget* color_dock_ = nullptr;
    QDockWidget* color_left_dock_ = nullptr;
    QDockWidget* color_nodes_dock_ = nullptr;
    QDockWidget* color_effects_dock_ = nullptr;
    QDockWidget* color_lightbox_dock_ = nullptr;

    ProjectManagerWindow* project_manager_window_ = nullptr;
    bool project_screen_active_ = false;

    friend void build_app_menus(MainWindow& main_window);
    friend QWidget* build_top_bar(MainWindow& main_window);
    friend void build_page_bar(MainWindow& main_window);
    friend QWidget* build_transport_bar(MainWindow& main_window);
    friend void build_left_dock(MainWindow& main_window);
    friend void build_inspector_dock(MainWindow& main_window);
    friend void build_deliver_docks(MainWindow& main_window);
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
    friend void apply_inspector_voice_isolation(MainWindow& main_window);
    friend void build_inspector_transition(MainWindow& main_window, QVBoxLayout* transition_layout,
                                           QToolButton* transition_mode_btn);
    friend void attach_inspector_transition(MainWindow& main_window, TimelineWidget* timeline);
    friend void update_inspector_transition(MainWindow& main_window);
    friend void apply_inspector_transition(MainWindow& main_window);
    friend void build_inspector_file(MainWindow& main_window, QVBoxLayout* file_layout);
    friend void attach_inspector_file(MainWindow& main_window, TimelineWidget* timeline);
    friend void update_inspector_file(MainWindow& main_window);
    friend void apply_inspector_file(MainWindow& main_window);
    friend void build_inspector_subtitles(MainWindow& main_window, QVBoxLayout* subtitles_layout);
    friend void attach_inspector_subtitles(MainWindow& main_window, TimelineWidget* timeline);
    friend void update_inspector_subtitles(MainWindow& main_window);
    friend void apply_inspector_subtitles(MainWindow& main_window);
    friend void build_center_workspace(MainWindow& main_window);
    friend void apply_view_options(MainWindow& main_window);
    friend void attach_timeline_view_options_button(MainWindow& main_window, QToolButton* button);
    friend void build_color_page(MainWindow& main_window);
    friend void enter_color_page(MainWindow& main_window);
    friend void leave_color_page(MainWindow& main_window);

    std::unique_ptr<canvas::core::Project> project_;
    canvas::core::UndoStack undo_;
    QString project_path_;
    QTimer* autosave_timer_ = nullptr;

    double fps_ = 30.0;
    int64_t total_frames_ = -1;
    int64_t current_frame_ = 0;
    bool has_unsaved_changes_ = false;
    bool source_hovering_ = false;
    canvas::core::ClipId selected_clip_ = 0;
    std::vector<canvas::core::ClipId> selected_clip_ids_;
    std::thread subtitle_worker_;
    std::atomic_bool subtitle_busy_{false};
    QPointer<SubtitleDialog> subtitle_dialog_;
    QString subtitle_media_path_;
    int subtitle_media_id_ = -1;
    int64_t subtitle_tl_in_ = 0;
    int64_t subtitle_tl_out_ = 0;
    int64_t subtitle_src_in_ = 0;
    double subtitle_seq_fps_ = 30.0;
    double subtitle_media_fps_ = 30.0;
    std::string subtitle_language_;
    canvas::core::captions::Options subtitle_opts_;
    std::shared_ptr<canvas::core::transcribe::Progress> subtitle_progress_;
    QElapsedTimer subtitle_elapsed_;
    std::string subtitle_model_name_;
    QDoubleSpinBox* inspector_audio_volume_ = nullptr;
    QDoubleSpinBox* inspector_audio_pan_ = nullptr;
};

}
