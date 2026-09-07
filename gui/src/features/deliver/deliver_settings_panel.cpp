#include "features/deliver/deliver_settings_panel.hpp"

#include "UX/theme.hpp"

#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFrame>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QSpinBox>
#include <QStringList>
#include <QStandardPaths>
#include <QTabWidget>
#include <QVBoxLayout>

#include <algorithm>
#include <string>
#include <vector>

namespace canvas::gui {

namespace {

void style_field(QWidget* w) {
    w->setStyleSheet(QStringLiteral(
        "QComboBox,QSpinBox,QDoubleSpinBox,QLineEdit{background:#0C0E14;color:#E8EAF0;"
        "border:1px solid #232833;border-radius:8px;padding:4px 8px;font-size:11px;}"));
}

QWidget* make_row(const QString& label, QWidget* field, QWidget* parent = nullptr) {
    auto* row = new QWidget(parent);
    auto* l = new QHBoxLayout(row);
    l->setContentsMargins(0, 0, 0, 0);
    l->setSpacing(6);
    auto* lbl = new QLabel(label, row);
    lbl->setMinimumWidth(120);
    lbl->setStyleSheet(QStringLiteral("color:#9AA0B0;font-size:11px;"));
    l->addWidget(lbl);
    l->addWidget(field, 1);
    return row;
}

QWidget* make_check(const QString& label, QWidget* parent = nullptr) {
    auto* c = new QCheckBox(label, parent);
    c->setStyleSheet(QStringLiteral("QCheckBox{color:#C6CAD6;font-size:11px;}"));
    return c;
}

QScrollArea* make_scroll(QWidget* content) {
    auto* sa = new QScrollArea;
    sa->setWidgetResizable(true);
    sa->setFrameShape(QFrame::NoFrame);
    sa->setStyleSheet(QStringLiteral("QScrollArea{background:#141A21;}"));
    sa->setWidget(content);
    return sa;
}

// A grouped settings section: a recessed semi-rounded card with a small caps
// header, appended to the given page layout. Returns the card's inner layout
// for the caller to fill with rows.
QVBoxLayout* make_section(const QString& title, QVBoxLayout* page) {
    auto* card = new QWidget;
    card->setStyleSheet(QStringLiteral(
        "QWidget{background:#0E1117;border:1px solid #232833;border-radius:10px;}"));
    auto* inner = new QVBoxLayout(card);
    inner->setContentsMargins(10, 8, 10, 10);
    inner->setSpacing(6);
    auto* t = new QLabel(title, card);
    t->setStyleSheet(QStringLiteral("color:#9AA0B0;font-size:10px;font-weight:600;"
                                    "padding:0 2px;"));
    inner->addWidget(t);
    page->addWidget(card);
    return inner;
}

}  // namespace

DeliverSettingsPanel::DeliverSettingsPanel(QWidget* parent) : QWidget(parent) {
    build();
    connect_all();
}

void DeliverSettingsPanel::build() {
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    // Header: preset + scope + file name/location. Raised card band with a
    // hairline, matching the inspector / dock-title surfaces.
    auto* header = new QWidget(this);
    header->setStyleSheet(QStringLiteral("background:#1A1D27;border-bottom:1px solid #232833;"));
    auto* h = new QVBoxLayout(header);
    h->setContentsMargins(10, 8, 10, 8);
    h->setSpacing(6);

    auto* preset_row = new QHBoxLayout;
    auto* preset_lbl = new QLabel(tr("Preset"));
    preset_lbl->setStyleSheet(QStringLiteral("color:#9AA0B0;font-size:11px;"));
    preset_combo_ = new QComboBox(header);
    preset_combo_->addItems({tr("Custom Export"), tr("YouTube 2160p"), tr("YouTube 1440p"),
                             tr("YouTube 1080p"), tr("Vimeo 4K"), tr("H.265 MKV Best"),
                             tr("H.264 MP4 Web")});
    style_field(preset_combo_);
    preset_row->addWidget(preset_lbl);
    preset_row->addWidget(preset_combo_, 1);
    h->addLayout(preset_row);

    scope_combo_ = new QComboBox(header);
    scope_combo_->addItems({tr("Single clip"), tr("Individual clips")});
    style_field(scope_combo_);
    h->addWidget(scope_combo_);

    auto* fn_row = new QHBoxLayout;
    auto* fn_lbl = new QLabel(tr("File Name"));
    fn_lbl->setStyleSheet(QStringLiteral("color:#9AA0B0;font-size:11px;"));
    file_name_ = new QLineEdit(header);
    file_name_->setText(tr("Untitled"));
    style_field(file_name_);
    fn_row->addWidget(fn_lbl);
    fn_row->addWidget(file_name_, 1);
    h->addLayout(fn_row);

    auto* loc_row = new QHBoxLayout;
    auto* loc_lbl = new QLabel(tr("Location"));
    loc_lbl->setStyleSheet(QStringLiteral("color:#9AA0B0;font-size:11px;"));
    location_ = new QLineEdit(header);
    {
        // Auto-detect the user's own Movies dir (falls back to the home dir when
        // the platform has none registered). Never a hard-coded username.
        const QString movies = QStandardPaths::writableLocation(QStandardPaths::MoviesLocation);
        location_->setText(movies.isEmpty() ? QDir::homePath() : movies);
    }
    style_field(location_);
    loc_row->addWidget(loc_lbl);
    loc_row->addWidget(location_, 1);
    location_browse_ = new QPushButton(tr("Browse..."), header);
    location_browse_->setStyleSheet(QStringLiteral(
        "QPushButton{background:#1A1D27;color:#C6CAD6;border:1px solid #232833;"
        "border-radius:8px;padding:4px 10px;font-size:11px;}"
        "QPushButton:hover{background:#20242F;}"));
    loc_row->addWidget(location_browse_);
    h->addLayout(loc_row);

    root->addWidget(header);

    tabs_ = new QTabWidget(this);
    tabs_->setStyleSheet(QStringLiteral(
        "QWidget#qt_tabwidget_stackedwidget{background:#141A21;}"
        "QTabWidget::pane{background:#141A21;border:none;}"
        "QTabBar::tab{background:transparent;color:#9AA0B0;padding:6px 14px;"
        "  border:none;border-radius:8px;font-weight:500;margin:2px 1px;}"
        "QTabBar::tab:selected{color:#FFFFFF;background:#3B82F6;font-weight:600;}"
        "QTabBar::tab:hover{color:#E8EAF0;}"
        "QTabBar::tab:selected:hover{color:#FFFFFF;}"));
    root->addWidget(tabs_, 1);

    // ---------------- VIDEO TAB ----------------
    auto* video = new QWidget;
    auto* v = new QVBoxLayout(video);
    v->setContentsMargins(12, 10, 12, 10);
    v->setSpacing(8);

    export_video_ = (QCheckBox*)make_check(tr("Export Video"));
    export_video_->setChecked(true);   // video exported by default

    format_combo_ = new QComboBox;
    for (const auto& f : canvas::core::deliver_formats()) format_combo_->addItem(QString::fromStdString(f));
    style_field(format_combo_);

    codec_combo_ = new QComboBox;
    for (const auto& c : canvas::core::deliver_video_codecs()) codec_combo_->addItem(QString::fromStdString(c));
    style_field(codec_combo_);

    encoder_combo_ = new QComboBox;
    style_field(encoder_combo_);
    rebuild_encoder_list();

    network_opt_ = (QCheckBox*)make_check(tr("Network Optimization"));
    {
        auto* sec = make_section(tr("OUTPUT"), v);
        sec->addWidget(export_video_);
        sec->addWidget(make_row(tr("Format"), format_combo_));
        sec->addWidget(make_row(tr("Codec"), codec_combo_));
        sec->addWidget(make_row(tr("Encoder"), encoder_combo_));
        sec->addWidget(network_opt_);
    }

    resolution_combo_ = new QComboBox;
    resolution_combo_->addItem(tr("Timeline Resolution"));
    resolution_combo_->addItem(tr("3840 x 2160"));
    resolution_combo_->addItem(tr("2560 x 1440"));
    resolution_combo_->addItem(tr("1920 x 1080"));
    resolution_combo_->addItem(tr("1280 x 720"));
    resolution_combo_->addItem(tr("Custom"));
    style_field(resolution_combo_);

    auto* res_row = new QHBoxLayout;
    res_w_ = new QSpinBox;
    res_w_->setRange(16, 7680);
    res_w_->setValue(2560);
    style_field(res_w_);
    res_h_ = new QSpinBox;
    res_h_->setRange(16, 4320);
    res_h_->setValue(1440);
    style_field(res_h_);
    res_row->addWidget(new QLabel(tr("W")));
    res_row->addWidget(res_w_, 1);
    res_row->addWidget(new QLabel(tr("H")));
    res_row->addWidget(res_h_, 1);
    vertical_res_ = new QCheckBox(tr("Use vertical res"));
    vertical_res_->setStyleSheet(QStringLiteral("QCheckBox{color:#C6CAD6;font-size:11px;}"));
    res_row->addWidget(vertical_res_);
    res_row->setContentsMargins(0, 0, 0, 0);

    auto* fr_row = new QWidget;
    auto* fr = new QHBoxLayout(fr_row);
    fr->setContentsMargins(0, 0, 0, 0);
    fr->setSpacing(6);
    custom_fps_chk_ = new QCheckBox(tr("Custom Frame Rate"));
    custom_fps_chk_->setStyleSheet(QStringLiteral("QCheckBox{color:#C6CAD6;font-size:11px;}"));
    custom_fps_chk_->setChecked(false);
    fr->addWidget(custom_fps_chk_);
    fr->addStretch(1);

    fps_spin_ = new QDoubleSpinBox;
    fps_spin_->setRange(1.0, 240.0);
    fps_spin_->setValue(30.0);
    fps_spin_->setEnabled(false);
    style_field(fps_spin_);
    {
        auto* sec = make_section(tr("RESOLUTION & FRAME RATE"), v);
        sec->addWidget(make_row(tr("Resolution"), resolution_combo_));
        sec->addLayout(res_row);
        sec->addWidget(fr_row);
        sec->addWidget(make_row(tr("FPS"), fps_spin_));
    }

    export_alpha_ = (QCheckBox*)make_check(tr("Export Alpha"));
    chapters_ = (QCheckBox*)make_check(tr("Chapters from Markers"));

    profile_combo_ = new QComboBox;
    profile_combo_->addItems({tr("Main"), tr("Main10"), tr("Main 4:2:2"), tr("Main 4:2:2 10"),
                              tr("Main 4:4:4"), tr("Main 4:4:4 10")});
    style_field(profile_combo_);

    key_frames_combo_ = new QComboBox;
    key_frames_combo_->addItem(tr("Automatic"));
    key_frames_combo_->addItem(tr("Every 30 frames"));
    style_field(key_frames_combo_);

    key_interval_spin_ = new QSpinBox;
    key_interval_spin_->setRange(1, 600);
    key_interval_spin_->setValue(30);
    style_field(key_interval_spin_);

    frame_reorder_ = (QCheckBox*)make_check(tr("Frame reordering"));

    preset_q_combo_ = new QComboBox;
    preset_q_combo_->addItems({tr("Placebo"), tr("Very Slow"), tr("Slow"), tr("Medium"),
                               tr("Fast"), tr("Faster"), tr("Very Fast"), tr("Superfast"),
                               tr("Ultrafast")});
    preset_q_combo_->setCurrentText(tr("Faster"));
    style_field(preset_q_combo_);

    tuning_combo_ = new QComboBox;
    tuning_combo_->addItems({tr("High Quality"), tr("Low Latency"), tr("Ultra Low Latency"),
                             tr("Lossless")});
    style_field(tuning_combo_);

    two_pass_ = (QCheckBox*)make_check(tr("Two Pass"));
    {
        auto* sec = make_section(tr("ENCODING"), v);
        sec->addWidget(make_row(tr("Encoding Profile"), profile_combo_));
        sec->addWidget(make_row(tr("Key Frames"), key_frames_combo_));
        sec->addWidget(make_row(tr("Key Frame Interval"), key_interval_spin_));
        sec->addWidget(frame_reorder_);
        sec->addWidget(make_row(tr("Preset"), preset_q_combo_));
        sec->addWidget(make_row(tr("Tuning"), tuning_combo_));
        sec->addWidget(two_pass_);
    }

    rate_control_combo_ = new QComboBox;
    rate_control_combo_->addItems({tr("Constant QP"), tr("Variable Bitrate (Quality)"),
                                   tr("Variable Bitrate (Target Kbps)"), tr("Constant Bitrate")});
    rate_control_combo_->setCurrentIndex(3);
    style_field(rate_control_combo_);

    quality_combo_ = new QComboBox;
    quality_combo_->addItems({tr("Best"), tr("High Quality"), tr("Good"), tr("Balanced"),
                              tr("Low"), tr("Lowest")});
    quality_combo_->setCurrentText(tr("Best"));
    style_field(quality_combo_);

    bitrate_row_ = new QWidget(this);
    {
        auto* row = new QHBoxLayout(bitrate_row_);
        row->setContentsMargins(0, 0, 0, 0);
        bitrate_label_ = new QLabel(tr("Bit Rate"), bitrate_row_);
        bitrate_label_->setStyleSheet(QStringLiteral("color:#C6CAD6;font-size:12px;"));
        row->addWidget(bitrate_label_);
        bitrate_spin_ = new QSpinBox(bitrate_row_);
        bitrate_spin_->setRange(0, 400000);
        bitrate_spin_->setValue(80000);
        bitrate_spin_->setSuffix(tr(" kb/s"));
        style_field(bitrate_spin_);
        row->addWidget(bitrate_spin_, 1);
    }

    max_bitrate_row_ = new QWidget(this);
    {
        auto* row = new QHBoxLayout(max_bitrate_row_);
        row->setContentsMargins(0, 0, 0, 0);
        row->addWidget(new QLabel(tr("Max (Kbps)"), max_bitrate_row_));
        max_bitrate_spin_ = new QSpinBox(max_bitrate_row_);
        max_bitrate_spin_->setRange(0, 400000);
        max_bitrate_spin_->setValue(80000);
        max_bitrate_spin_->setSuffix(tr(" kb/s"));
        style_field(max_bitrate_spin_);
        row->addWidget(max_bitrate_spin_, 1);
    }

    multi_encode_combo_ = new QComboBox;
    multi_encode_combo_->addItems({tr("Auto"), tr("Enabled"), tr("Disabled")});
    style_field(multi_encode_combo_);
    {
        auto* sec = make_section(tr("QUALITY & BITRATE"), v);
        sec->addWidget(make_row(tr("Rate Control"), rate_control_combo_));
        sec->addWidget(make_row(tr("Quality"), quality_combo_));
        sec->addWidget(bitrate_row_);
        sec->addWidget(max_bitrate_row_);
        sec->addWidget(make_row(tr("Multi Encode"), multi_encode_combo_));
    }

    // Bitrate fields are only meaningful for bitrate-driven modes (Constant
    // Bitrate / VBR target). For quality modes they are hidden; for CBR only the
    // single "Bit Rate" field is shown (target == max raised here).
    update_bitrate_visibility();

    lookahead_spin_ = new QSpinBox;
    lookahead_spin_->setRange(0, 64);
    lookahead_spin_->setValue(16);
    style_field(lookahead_spin_);

    lookahead_level_ = new QSpinBox;
    lookahead_level_->setRange(0, 6);
    lookahead_level_->setValue(0);
    style_field(lookahead_level_);

    scene_cut_ = (QCheckBox*)make_check(tr("Disable adaptive I-frame at scene cuts"));
    adaptive_b_ = (QCheckBox*)make_check(tr("Enable adaptive B-frame"));
    aq_strength_ = new QSpinBox;
    aq_strength_->setRange(0, 16);
    aq_strength_->setValue(8);
    style_field(aq_strength_);
    nref_p_ = (QCheckBox*)make_check(tr("Enable non-reference P-frame"));
    weighted_pred_ = (QCheckBox*)make_check(tr("Enable weighted prediction"));
    temporal_filt_ = (QCheckBox*)make_check(tr("Temporal Filtering"));
    uni_b_ = (QCheckBox*)make_check(tr("Unidirection B Frames"));
    {
        auto* sec = make_section(tr("ADVANCED"), v);
        sec->addWidget(make_row(tr("Lookahead"), lookahead_spin_));
        sec->addWidget(make_row(tr("Lookahead Level"), lookahead_level_));
        sec->addWidget(make_row(tr("AQ Strength"), aq_strength_));
        sec->addWidget(scene_cut_);
        sec->addWidget(adaptive_b_);
        sec->addWidget(nref_p_);
        sec->addWidget(weighted_pred_);
        sec->addWidget(temporal_filt_);
        sec->addWidget(uni_b_);
    }

    {
        auto* sec = make_section(tr("ALPHA & MARKERS"), v);
        sec->addWidget(export_alpha_);
        sec->addWidget(chapters_);
    }

    v->addStretch(1);
    tabs_->addTab(make_scroll(video), tr("Video"));

    // ---------------- AUDIO TAB ----------------
    auto* audio = new QWidget;
    auto* au = new QVBoxLayout(audio);
    au->setContentsMargins(12, 10, 12, 10);
    au->setSpacing(8);

    export_audio_ = (QCheckBox*)make_check(tr("Export Audio"));
    export_audio_->setChecked(true);   // audio exported by default

    audio_codec_combo_ = new QComboBox;
    for (const auto& c : canvas::core::deliver_audio_codecs()) audio_codec_combo_->addItem(QString::fromStdString(c));
    style_field(audio_codec_combo_);

    audio_bitrate_ = new QSpinBox;
    audio_bitrate_->setRange(32, 512);
    audio_bitrate_->setValue(192);
    style_field(audio_bitrate_);

    audio_rate_combo_ = new QComboBox;
    audio_rate_combo_->addItems({tr("44100"), tr("48000"), tr("96000")});
    audio_rate_combo_->setCurrentText(tr("48000"));
    style_field(audio_rate_combo_);

    audio_channels_combo_ = new QComboBox;
    audio_channels_combo_->addItems({tr("Mono"), tr("Stereo"), tr("5.1")});
    audio_channels_combo_->setCurrentText(tr("Stereo"));
    style_field(audio_channels_combo_);
    {
        auto* sec = make_section(tr("AUDIO"), au);
        sec->addWidget(export_audio_);
        sec->addWidget(make_row(tr("Codec"), audio_codec_combo_));
        sec->addWidget(make_row(tr("Bitrate (Kbps)"), audio_bitrate_));
        sec->addWidget(make_row(tr("Sample Rate"), audio_rate_combo_));
        sec->addWidget(make_row(tr("Channels"), audio_channels_combo_));
    }

    au->addStretch(1);
    tabs_->addTab(make_scroll(audio), tr("Audio"));

    // ---------------- FILE / ADVANCED TAB ----------------
    auto* file = new QWidget;
    auto* f = new QVBoxLayout(file);
    f->setContentsMargins(12, 10, 12, 10);
    f->setSpacing(8);

    pixel_aspect_combo_ = new QComboBox;
    pixel_aspect_combo_->addItems({tr("Square"), tr("Cinemascope")});
    style_field(pixel_aspect_combo_);

    data_levels_combo_ = new QComboBox;
    data_levels_combo_->addItems({tr("Auto"), tr("Video"), tr("Full")});
    style_field(data_levels_combo_);

    retain_sub_black_ = (QCheckBox*)make_check(tr("Retain sub-black and super-white data"));

    color_space_combo_ = new QComboBox;
    color_space_combo_->addItems({tr("Same as project"), tr("Rec.709"), tr("Rec.2020"),
                                  tr("DCI-P3")});
    style_field(color_space_combo_);

    gamma_combo_ = new QComboBox;
    gamma_combo_->addItems({tr("Same as project"), tr("sRGB"), tr("Gamma 2.4")});
    style_field(gamma_combo_);

    data_burn_in_combo_ = new QComboBox;
    data_burn_in_combo_->addItems({tr("Same as project"), tr("Off")});
    style_field(data_burn_in_combo_);
    {
        auto* sec = make_section(tr("COLOR & LEVELS"), f);
        sec->addWidget(make_row(tr("Pixel aspect ratio"), pixel_aspect_combo_));
        sec->addWidget(make_row(tr("Data Levels"), data_levels_combo_));
        sec->addWidget(retain_sub_black_);
        sec->addWidget(make_row(tr("Color Space Tag"), color_space_combo_));
        sec->addWidget(make_row(tr("Gamma Tag"), gamma_combo_));
        sec->addWidget(make_row(tr("Data burn-in"), data_burn_in_combo_));
    }

    bypass_reencode_ = (QCheckBox*)make_check(tr("Bypass re-encode when possible"));
    render_all_tracks_ = (QCheckBox*)make_check(tr("Render All Video Tracks"));
    force_sizing_hq_ = (QCheckBox*)make_check(tr("Force sizing to highest quality"));
    force_debayer_hq_ = (QCheckBox*)make_check(tr("Force debayer to highest quality"));

    flat_pass_combo_ = new QComboBox;
    flat_pass_combo_->addItems({tr("Off"), tr("On")});
    style_field(flat_pass_combo_);

    visionos_combo_ = new QComboBox;
    visionos_combo_->addItems({tr("Off"), tr("On")});
    style_field(visionos_combo_);
    {
        auto* sec = make_section(tr("PROCESSING"), f);
        sec->addWidget(bypass_reencode_);
        sec->addWidget(render_all_tracks_);
        sec->addWidget(force_sizing_hq_);
        sec->addWidget(force_debayer_hq_);
        sec->addWidget(make_row(tr("Enable Flat Pass"), flat_pass_combo_));
        sec->addWidget(make_row(tr("visionOS Bypass"), visionos_combo_));
    }

    auto* buttons = new QHBoxLayout;
    estimate_label_ = new QLabel(tr("Estimated File Size: --"));
    estimate_label_->setStyleSheet(QStringLiteral("color:#8FCBFF;font-size:11px;"));
    buttons->addWidget(estimate_label_, 1);

    auto* add_btn = new QPushButton(tr("Add to Render Queue"));
    add_btn->setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Fixed);
    add_btn->setStyleSheet(QStringLiteral("QPushButton{background:#3B82F6;color:white;border-radius:8px;"
                                          "padding:7px 14px;font-weight:600;}"
                                          "QPushButton:hover{background:#4C92FF;}"));
    connect(add_btn, &QPushButton::clicked, this, &DeliverSettingsPanel::add_to_queue_clicked);
    buttons->addWidget(add_btn);

    auto* footer = new QWidget(this);
    footer->setStyleSheet(QStringLiteral("background:#1A1D27;border-top:1px solid #232833;"));
    auto* fo = new QVBoxLayout(footer);
    fo->setContentsMargins(10, 8, 10, 8);
    fo->setSpacing(4);
    fo->addLayout(buttons);
    root->addWidget(footer);
    f->addStretch(1);
    tabs_->addTab(make_scroll(file), tr("File"));

    // Confine codec/audio-codec choices to what the current container supports
    // (format-dependent), so users can't create jobs the muxer rejects.
    rebuild_codec_list();
}

void DeliverSettingsPanel::rebuild_encoder_list() {
    const QString cur = encoder_combo_ ? encoder_combo_->currentText() : QString();
    encoder_combo_->clear();
    for (const auto& e : canvas::core::deliver_encoders()) encoder_combo_->addItem(QString::fromStdString(e));
    if (!cur.isEmpty()) encoder_combo_->setCurrentText(cur);
}

namespace {

// Codecs that can actually go in a given container (validated against FFmpeg).
// This mirrors what real NLEs expose: the codec list is restricted so users
// can't pick a combination the muxer rejects (WebM+H.264, MP4+ProRes, ...).
QStringList video_codecs_for_format(const QString& format) {
    const QString fmt = format.toLower();
    if (fmt.contains("mkv"))
        return {"H.264", "H.265", "AV1", "Apple ProRes", "FFV1", "JPEG 2000", "Uncompressed"};
    if (fmt.contains("mp4"))
        return {"H.264", "H.265", "AV1"};
    if (fmt.contains("quicktime") || fmt == "mov")
        return {"H.264", "H.265", "Apple ProRes", "FFV1", "Uncompressed"};
    if (fmt == "webm")
        return {"AV1"};  // WebM allows VP8/VP9/AV1; we expose AV1 here.
    if (fmt.contains("avi"))
        return {"H.264", "H.265", "FFV1", "Uncompressed"};
    if (fmt.contains("mxf") || fmt.contains("imf"))
        return {"H.264", "H.265"};
    if (fmt.contains("mpeg-2") || fmt == "mpeg")
        return {"H.264"};
    // Image-sequence formats: codec is irrelevant at the muxer level.
    if (fmt.contains("png") || fmt.contains("dpx") || fmt.contains("exr") ||
        fmt.contains("jpeg") || fmt.contains("tiff") || fmt.contains("webp") ||
        fmt.contains("gif"))
        return {"Uncompressed"};
    return {"H.264", "H.265", "AV1"};
}

QStringList audio_codecs_for_format(const QString& format) {
    const QString fmt = format.toLower();
    if (fmt == "webm")
        return {"Opus", "Vorbis"};
    if (fmt.contains("mpeg-2") || fmt == "mpeg")
        return {"MP3"};
    if (fmt.contains("avi"))
        return {"PCM", "MP3"};
    if (fmt.contains("mxf") || fmt.contains("imf"))
        return {"PCM"};
    return {"AAC", "MP3", "PCM", "FLAC", "Opus", "Vorbis"};
}

}  // namespace

void DeliverSettingsPanel::rebuild_codec_list() {
    if (!format_combo_ || !codec_combo_) return;

    const QString fmt = format_combo_->currentText();
    const QString cur_v = codec_combo_->currentText();

    codec_combo_->clear();
    const QStringList vcs = video_codecs_for_format(fmt);
    for (const QString& c : vcs) codec_combo_->addItem(c);
    if (!cur_v.isEmpty() && vcs.contains(cur_v))
        codec_combo_->setCurrentText(cur_v);
    else if (!vcs.isEmpty())
        codec_combo_->setCurrentText(vcs.first());

    if (audio_codec_combo_) {
        const QString cur_a = audio_codec_combo_->currentText();
        audio_codec_combo_->clear();
        const QStringList acs = audio_codecs_for_format(fmt);
        for (const QString& c : acs) audio_codec_combo_->addItem(c);
        if (!cur_a.isEmpty() && acs.contains(cur_a))
            audio_codec_combo_->setCurrentText(cur_a);
        else if (!acs.isEmpty())
            audio_codec_combo_->setCurrentText(acs.first());
    }

    // The codec list drives the encoder backend choices too (e.g. AV1 in WebM
    // actually routes through libsvtav1/av1_nvenc); re-sync the encoder list.
    rebuild_encoder_list();
}

void DeliverSettingsPanel::connect_all() {
    auto onChange = [this] { emit settings_changed(); update_estimate(); };
    for (QComboBox* cb : {preset_combo_, scope_combo_, format_combo_, codec_combo_, encoder_combo_,
                          resolution_combo_, profile_combo_, key_frames_combo_,
                          rate_control_combo_, quality_combo_, multi_encode_combo_, preset_q_combo_,
                          tuning_combo_,
                          audio_codec_combo_, audio_rate_combo_, audio_channels_combo_,
                          pixel_aspect_combo_, data_levels_combo_, color_space_combo_, gamma_combo_,
                          data_burn_in_combo_, flat_pass_combo_, visionos_combo_})
        connect(cb, &QComboBox::currentIndexChanged, this, onChange);
    // Switching rate-control mode reshows/relabels the bitrate fields.
    connect(rate_control_combo_, &QComboBox::currentIndexChanged, this,
            [this] { update_bitrate_visibility(); });
    for (QCheckBox* c : {export_video_, network_opt_, vertical_res_, export_alpha_, chapters_,
                         custom_fps_chk_,
                         frame_reorder_, two_pass_, scene_cut_, adaptive_b_, nref_p_, weighted_pred_,
                         temporal_filt_, uni_b_, export_audio_, retain_sub_black_, bypass_reencode_,
                         render_all_tracks_, force_sizing_hq_, force_debayer_hq_})
        connect(c, &QCheckBox::toggled, this, onChange);
    for (QSpinBox* s : {res_w_, res_h_, key_interval_spin_, bitrate_spin_,
                        max_bitrate_spin_, lookahead_spin_, lookahead_level_, aq_strength_,
                        audio_bitrate_})
        connect(s, QOverload<int>::of(&QSpinBox::valueChanged), this, onChange);
    connect(fps_spin_, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, onChange);
    connect(custom_fps_chk_, &QCheckBox::toggled, this, [this](bool on) {
        fps_spin_->setEnabled(on);
        emit settings_changed();
    });
    connect(file_name_, &QLineEdit::textChanged, this, onChange);
    connect(location_, &QLineEdit::textChanged, this, onChange);
    connect(location_browse_, &QPushButton::clicked, this, [this] {
        const QString dir = QFileDialog::getExistingDirectory(
            this, tr("Choose Output Folder"),
            location_->text().trimmed().isEmpty() ? QDir::homePath() : location_->text());
        if (!dir.isEmpty()) location_->setText(dir);
    });

    // Changing the container format restricts the available codecs/audio codecs.
    connect(format_combo_, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            [this] { rebuild_codec_list(); emit settings_changed(); });
}

void DeliverSettingsPanel::update_bitrate_visibility() {
    if (!bitrate_spin_ || !max_bitrate_spin_) return;
    const int rc = rate_control_combo_->currentIndex();
    // RateControl: 0=ConstantQP, 1=VBR(Quality), 2=VBR(Target), 3=ConstantBitrate.
    const bool show_bitrate = (rc == 2 || rc == 3);
    const bool show_max = (rc == 2);
    if (rc == 3) {
        bitrate_label_->setText(tr("Bit Rate"));
        max_bitrate_spin_->setValue(bitrate_spin_->value());
    } else if (rc == 2) {
        bitrate_label_->setText(tr("Target (Kbps)"));
    } else {
        bitrate_label_->setText(tr("Bit Rate"));
    }
    bitrate_row_->setVisible(show_bitrate);
    max_bitrate_row_->setVisible(show_max);
}

canvas::core::DeliverSettings DeliverSettingsPanel::settings() const {
    canvas::core::DeliverSettings ds;
    ds.preset_name = preset_combo_->currentText().toStdString();
    ds.render_scope = scope_combo_->currentIndex() == 0 ? canvas::core::RenderScope::SingleClip
                                                        : canvas::core::RenderScope::IndividualClips;
    ds.file.file_name = file_name_->text().toStdString();
    ds.file.location = location_->text().toStdString();

    ds.video.export_video = export_video_->isChecked();
    ds.video.format = format_combo_->currentText().toStdString();
    ds.video.codec = codec_combo_->currentText().toStdString();
    const QString enc = encoder_combo_->currentText();
    if (enc == tr("Auto")) ds.video.encoder = canvas::core::EncoderBackend::Auto;
    else if (enc == tr("CPU")) ds.video.encoder = canvas::core::EncoderBackend::CPU;
    else if (enc == tr("NVIDIA")) ds.video.encoder = canvas::core::EncoderBackend::NVIDIA;
    else if (enc == tr("AMD")) ds.video.encoder = canvas::core::EncoderBackend::AMD;
    else if (enc == tr("Intel")) ds.video.encoder = canvas::core::EncoderBackend::Intel;
    ds.video.network_optimization = network_opt_->isChecked();

    const QString res = resolution_combo_->currentText();
    if (res == tr("Timeline Resolution")) { ds.video.resolution = "Timeline Resolution"; }
    else { ds.video.resolution = res.toStdString(); }
    if (res == tr("3840 x 2160")) { ds.video.custom_width = 3840; ds.video.custom_height = 2160; }
    else if (res == tr("2560 x 1440")) { ds.video.custom_width = 2560; ds.video.custom_height = 1440; }
    else if (res == tr("1920 x 1080")) { ds.video.custom_width = 1920; ds.video.custom_height = 1080; }
    else if (res == tr("1280 x 720")) { ds.video.custom_width = 1280; ds.video.custom_height = 720; }
    else if (res == tr("Custom")) { ds.video.custom_width = res_w_->value(); ds.video.custom_height = res_h_->value(); }
    ds.video.use_vertical_resolution = vertical_res_->isChecked();

    // Frame rate: auto-detect from source media by default; a concrete value only
    // applies when "Custom Frame Rate" is explicitly enabled.
    if (custom_fps_chk_->isChecked()) {
        ds.video.frame_rate = "Custom";
        ds.video.custom_fps = fps_spin_->value();
    } else {
        ds.video.frame_rate = "Auto";  // resolved to the source media fps by the caller
        ds.video.custom_fps = 0.0;
    }

    ds.video.export_alpha = export_alpha_->isChecked();
    ds.video.chapters_from_markers = chapters_->isChecked();
    ds.video.encoding_profile = (canvas::core::EncodingProfile)profile_combo_->currentIndex();
    ds.video.key_frames = key_frames_combo_->currentIndex() == 0 ? canvas::core::KeyFrameMode::Automatic
                                                                  : canvas::core::KeyFrameMode::EveryNFrames;
    ds.video.key_frame_interval = key_interval_spin_->value();
    ds.video.frame_reordering = frame_reorder_->isChecked();
    ds.video.rate_control = (canvas::core::RateControl)rate_control_combo_->currentIndex();
    {
        const QString q = quality_combo_->currentText();
        if (q == tr("Best")) ds.video.quality = 0;
        else if (q == tr("High Quality")) ds.video.quality = 12;
        else if (q == tr("Good")) ds.video.quality = 18;
        else if (q == tr("Balanced")) ds.video.quality = 22;
        else if (q == tr("Low")) ds.video.quality = 28;
        else ds.video.quality = 34;
    }
    ds.video.target_bitrate_kbps = bitrate_spin_->value();
    ds.video.max_bitrate_kbps =
        (ds.video.rate_control == canvas::core::RateControl::ConstantBitrate)
            ? bitrate_spin_->value()
            : max_bitrate_spin_->value();
    ds.video.multi_encode = (canvas::core::MultiEncode)multi_encode_combo_->currentIndex();
    ds.video.preset = preset_q_combo_->currentText().toStdString();
    ds.video.tuning = (canvas::core::EncoderTuning)tuning_combo_->currentIndex();
    ds.video.two_pass = two_pass_->isChecked();
    ds.video.lookahead_frames = lookahead_spin_->value();
    ds.video.lookahead_level = lookahead_level_->value();
    ds.video.adaptive_i_at_scene_cuts = scene_cut_->isChecked();
    ds.video.adaptive_b_frame = adaptive_b_->isChecked();
    ds.video.aq_strength = aq_strength_->value();
    ds.video.non_reference_p_frame = nref_p_->isChecked();
    ds.video.weighted_prediction = weighted_pred_->isChecked();
    ds.video.temporal_filtering = temporal_filt_->isChecked();
    ds.video.unidirectional_b_frames = uni_b_->isChecked();

    ds.video.pixel_aspect = (canvas::core::PixelAspect)pixel_aspect_combo_->currentIndex();
    ds.video.data_levels = (canvas::core::DataLevels)data_levels_combo_->currentIndex();
    ds.video.retain_sub_black_super_white = retain_sub_black_->isChecked();
    ds.video.color_space_tag = color_space_combo_->currentText().toStdString();
    ds.video.gamma_tag = gamma_combo_->currentText().toStdString();
    ds.video.data_burn_in = data_burn_in_combo_->currentText().toStdString();
    ds.video.bypass_reenecode_when_possible = bypass_reencode_->isChecked();
    ds.video.render_all_video_tracks = render_all_tracks_->isChecked();
    ds.video.force_sizing_high_quality = force_sizing_hq_->isChecked();
    ds.video.force_debayer_high_quality = force_debayer_hq_->isChecked();
    ds.video.flat_pass = flat_pass_combo_->currentText().toStdString();
    ds.video.visionos_bypass = visionos_combo_->currentText().toStdString();

    ds.audio.export_audio = export_audio_->isChecked();
    ds.audio.codec = audio_codec_combo_->currentText().toStdString();
    ds.audio.bitrate_kbps = audio_bitrate_->value();
    ds.audio.sample_rate = audio_rate_combo_->currentText().toInt();
    ds.audio.channels = audio_channels_combo_->currentIndex() == 0 ? 1
                        : audio_channels_combo_->currentIndex() == 2 ? 6 : 2;

    return ds;
}

void DeliverSettingsPanel::set_settings(const canvas::core::DeliverSettings& ds) {
    building_ = true;
    preset_combo_->setCurrentText(QString::fromStdString(ds.preset_name));
    scope_combo_->setCurrentIndex(ds.render_scope == canvas::core::RenderScope::SingleClip ? 0 : 1);
    file_name_->setText(QString::fromStdString(ds.file.file_name));
    location_->setText(QString::fromStdString(ds.file.location));
    format_combo_->setCurrentText(QString::fromStdString(ds.video.format));
    codec_combo_->setCurrentText(QString::fromStdString(ds.video.codec));
    encoder_combo_->setCurrentIndex((int)ds.video.encoder);
    network_opt_->setChecked(ds.video.network_optimization);
    export_video_->setChecked(ds.video.export_video);
    export_audio_->setChecked(ds.audio.export_audio);
    building_ = false;
    emit settings_changed();
}

void DeliverSettingsPanel::set_timeline_length(double duration_seconds, double timeline_fps) {
    duration_seconds_ = duration_seconds;
    timeline_fps_ = timeline_fps;
    update_estimate();
}

void DeliverSettingsPanel::update_estimate() {
    const auto kColorStyle = QLatin1String("color:#8FCBFF;font-size:11px;");
    if (!estimate_label_ || duration_seconds_ <= 0.0) {
        estimate_label_->setText(tr("Estimated File Size: --"));
        estimate_label_->setStyleSheet(kColorStyle);
        return;
    }

    const canvas::core::DeliverSettings ds = settings();

    double video_kbps = 0.0;
    if (ds.video.export_video) {
        using RC = canvas::core::RateControl;
        switch (ds.video.rate_control) {
            case RC::ConstantBitrate:
            case RC::VBRTargetKbps:
                // VBR-target maxes at target_bitrate; use it as the estimate.
                video_kbps = ds.video.target_bitrate_kbps;
                break;
            case RC::ConstantQP:
            case RC::VBRQuality: {
                // No target bitrate in quality/CRF modes — guess from a rough
                // bits-per-pixel-per-frame figure per codec. Estimating is an
                // art; these land within ~40% for typical content.
                double bpp = 0.10;
                using VC = canvas::core::VideoCodec;
                switch (canvas::core::video_codec_from_string(ds.video.codec)) {
                    case VC::H264:       bpp = 0.10; break;
                    case VC::H265:       bpp = 0.065; break;
                    case VC::AV1:        bpp = 0.055; break;
                    case VC::ProRes:     bpp = 0.75; break;
                    case VC::FFV1:       bpp = 1.5; break;
                    case VC::JPEG2000:   bpp = 0.35; break;
                    case VC::Uncompressed: bpp = 24.0; break;
                }
                // Effective resolution: parse "W x H" from the combo, else fall
                // back to the custom width/height fields. Vertical mode swaps.
                long w = ds.video.custom_width;
                long h = ds.video.custom_height;
                const std::string res = ds.video.resolution;
                const std::size_t x = res.find('x');
                if (x != std::string::npos) {
                    try {
                        w = std::stol(res.substr(0, x));
                        h = std::stol(res.substr(x + 1));
                    } catch (...) {
                        // keep the custom fallback
                    }
                }
                if (ds.video.use_vertical_resolution) std::swap(w, h);
                const long px = std::max(w, 1L) * std::max(h, 1L);
                const double fps = ds.video.frame_rate == "Timeline Frame Rate"
                                       ? (timeline_fps_ > 0.0 ? timeline_fps_ : 30.0)
                                       : (ds.video.custom_fps > 0.0 ? ds.video.custom_fps : 30.0);
                video_kbps = px * bpp * fps / 1000.0;
                break;
            }
        }
    }

    double audio_kbps = 0.0;
    if (ds.audio.export_audio && !ds.audio.codec.empty())
        audio_kbps = ds.audio.bitrate_kbps;

    const double total_kbps = video_kbps + audio_kbps;
    const double total_mb = total_kbps * duration_seconds_ / 8.0 / 1024.0;

    QString size_text;
    if (total_mb >= 1024.0)
        size_text = tr("%1 GB").arg(total_mb / 1024.0, 0, 'f', 1);
    else
        size_text = tr("%1 MB").arg(total_mb, 0, 'f', 1);
    estimate_label_->setText(tr("Estimated File Size: %1").arg(size_text));
    estimate_label_->setStyleSheet(kColorStyle);
}

}  // namespace canvas::gui
