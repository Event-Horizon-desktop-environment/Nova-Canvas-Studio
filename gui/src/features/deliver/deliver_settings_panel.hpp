#pragma once

#include <QWidget>

#include "canvas/core/export/deliver_preset.hpp"

class QTabWidget;
class QComboBox;
class QCheckBox;
class QSpinBox;
class QDoubleSpinBox;
class QLineEdit;
class QPushButton;
class QLabel;

namespace canvas::gui {

// Left-hand render settings panel for the Deliver page (Resolve-style). Builds
// controls over canvas::core::DeliverSettings and emits readiness changes. The
// caller reads settings() after any change to build an export job.
class DeliverSettingsPanel : public QWidget {
    Q_OBJECT

public:
    explicit DeliverSettingsPanel(QWidget* parent = nullptr);

    // Current settings, reflecting all the widgets.
    canvas::core::DeliverSettings settings() const;

    // Sets the controls from a settings struct (e.g. loading a preset).
    void set_settings(const canvas::core::DeliverSettings& ds);

    // FPS counter for the running render (updated by the queue hook).
    void set_render_speed(double fps, bool gpu);

signals:
    void settings_changed();
    void add_to_queue_clicked();
    void render_all_clicked();

private:
    void build();
    void rebuild_encoder_list();
    void rebuild_codec_list();
    void connect_all();
    // Show/hide + relabel the bitrate controls to match the selected rate
    // control mode (CBR -> one "Bit Rate" field; VBR target -> +Max; quality -> none).
    void update_bitrate_visibility();

    QTabWidget* tabs_ = nullptr;
    QComboBox* preset_combo_ = nullptr;
    QComboBox* scope_combo_ = nullptr;
    QLineEdit* file_name_ = nullptr;
    QLineEdit* location_ = nullptr;
    QPushButton* location_browse_ = nullptr;

    // Video tab
    QCheckBox* export_video_ = nullptr;
    QComboBox* format_combo_ = nullptr;
    QComboBox* codec_combo_ = nullptr;
    QComboBox* encoder_combo_ = nullptr;
    QCheckBox* network_opt_ = nullptr;
    QComboBox* resolution_combo_ = nullptr;
    QSpinBox* res_w_ = nullptr;
    QSpinBox* res_h_ = nullptr;
    QCheckBox* vertical_res_ = nullptr;
    QCheckBox* custom_fps_chk_ = nullptr;
    QDoubleSpinBox* fps_spin_ = nullptr;
    QCheckBox* export_alpha_ = nullptr;
    QCheckBox* chapters_ = nullptr;
    QComboBox* profile_combo_ = nullptr;
    QComboBox* key_frames_combo_ = nullptr;
    QSpinBox* key_interval_spin_ = nullptr;
    QCheckBox* frame_reorder_ = nullptr;
    QComboBox* rate_control_combo_ = nullptr;
    QComboBox* quality_combo_ = nullptr;
    QWidget* bitrate_row_ = nullptr;
    QLabel* bitrate_label_ = nullptr;
    QSpinBox* bitrate_spin_ = nullptr;
    QWidget* max_bitrate_row_ = nullptr;
    QSpinBox* max_bitrate_spin_ = nullptr;
    QComboBox* multi_encode_combo_ = nullptr;
    QComboBox* preset_q_combo_ = nullptr;
    QComboBox* tuning_combo_ = nullptr;
    QCheckBox* two_pass_ = nullptr;
    QSpinBox* lookahead_spin_ = nullptr;
    QSpinBox* lookahead_level_ = nullptr;
    QCheckBox* scene_cut_ = nullptr;
    QCheckBox* adaptive_b_ = nullptr;
    QSpinBox* aq_strength_ = nullptr;
    QCheckBox* nref_p_ = nullptr;
    QCheckBox* weighted_pred_ = nullptr;
    QCheckBox* temporal_filt_ = nullptr;
    QCheckBox* uni_b_ = nullptr;

    // Audio tab
    QCheckBox* export_audio_ = nullptr;
    QComboBox* audio_codec_combo_ = nullptr;
    QSpinBox* audio_bitrate_ = nullptr;
    QComboBox* audio_rate_combo_ = nullptr;
    QComboBox* audio_channels_combo_ = nullptr;

    // File tab
    QComboBox* pixel_aspect_combo_ = nullptr;
    QComboBox* data_levels_combo_ = nullptr;
    QCheckBox* retain_sub_black_ = nullptr;
    QComboBox* color_space_combo_ = nullptr;
    QComboBox* gamma_combo_ = nullptr;
    QComboBox* data_burn_in_combo_ = nullptr;
    QCheckBox* bypass_reencode_ = nullptr;
    QCheckBox* render_all_tracks_ = nullptr;
    QCheckBox* force_sizing_hq_ = nullptr;
    QCheckBox* force_debayer_hq_ = nullptr;
    QComboBox* flat_pass_combo_ = nullptr;
    QComboBox* visionos_combo_ = nullptr;
    QLabel* estimate_label_ = nullptr;
    QLabel* speed_label_ = nullptr;

    bool building_ = false;
};

}  // namespace canvas::gui
