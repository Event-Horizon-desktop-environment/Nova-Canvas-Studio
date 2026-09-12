// File inspector page. Built once per MainWindow (registry keyed by window),
// refreshed from whichever clip is selected. Header info is read-only (what the
// model knows about the source); Metadata (timecode/tag/colour/name/notes) and
// the clip Name field commit live through edit_ops so every change is undoable.
// Audio Configuration lists one row per audio channel of the source; the
// per-channel audition buttons are not connected to a playback engine in this
// build (v1), so they render disabled with a tooltip.

#include "UX/InspectorFile.hpp"

#include <QCheckBox>
#include <QComboBox>
#include <QDesktopServices>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPainter>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QRegularExpression>
#include <QToolButton>
#include <QUrl>
#include <QVBoxLayout>

#include <cmath>
#include <functional>
#include <map>
#include <optional>
#include <vector>

#include "UX/InspectorShared.hpp"
#include "UX/MainWindow.hpp"
#include "UX/theme.hpp"
#include "Widgets/timeline_widget.hpp"
#include "canvas/core/project/project.hpp"
#include "canvas/core/timeline/edit_ops.hpp"
#include "canvas/core/timeline/model.hpp"
#include "core/timecode.hpp"

namespace canvas::gui {

namespace {

using canvas::core::Clip;
using canvas::core::MediaEntry;
using canvas::core::Track;

QString muted_label_style() {
    return QStringLiteral("color: %1; font-size: 11px;").arg(css(tokens().ink_muted));
}

QString faint_label_style() {
    return QStringLiteral("color: %1; font-size: 11px;").arg(css(tokens().ink_faint));
}

QString readonly_line_style() {
    const ThemeTokens& t = tokens();
    return QStringLiteral(
               "QLineEdit { background-color: %1; color: %2; border: 1px solid %3;"
               " border-radius: 8px; padding: 3px 6px; }")
        .arg(css(t.surface_low), css(t.ink_muted), css(t.border_soft));
}

QString editable_line_style() {
    const ThemeTokens& t = tokens();
    return QStringLiteral(
               "QLineEdit { background-color: %1; color: %2; border: 1px solid %3;"
               " border-radius: 8px; padding: 3px 6px; }"
               "QLineEdit:focus { border-color: %4; }")
        .arg(css(t.surface_higher), css(t.ink), css(t.border), css(t.accent));
}

QString notes_style() {
    const ThemeTokens& t = tokens();
    return QStringLiteral(
               "QPlainTextEdit { background-color: %1; color: %2; border: 1px solid %3;"
               " border-radius: 8px; padding: 4px 6px; }"
               "QPlainTextEdit:focus { border-color: %4; }")
        .arg(css(t.surface_higher), css(t.ink), css(t.border), css(t.accent));
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

QString preview_name_style() {
    return QStringLiteral("color: %1; font-size: 12px; font-weight: 600;")
        .arg(css(tokens().ink));
}

QString preview_card_style() {
    const ThemeTokens& t = tokens();
    return QStringLiteral("QWidget#inspectorPreviewCard { background-color: %1;"
                          " border: 1px solid %2; border-radius: 8px; }")
        .arg(css(t.surface_raised), css(t.border_soft));
}

QString no_color_style() {
    const ThemeTokens& t = tokens();
    return QStringLiteral(
               "QToolButton { background: %1; color: %2; border: 1px solid %3;"
               " border-radius: 4px; font-size: 9px; }"
               "QToolButton:hover { color: %4; }")
        .arg(css(t.surface_raised), css(t.ink_faint), css(t.border_soft), css(t.ink));
}

QString progress_style() {
    const ThemeTokens& t = tokens();
    return QStringLiteral(
               "QProgressBar { border: none; background: %1; border-radius: 3px; }"
               "QProgressBar::chunk { background: %2; border-radius: 3px; }")
        .arg(css(t.surface_low), css(t.accent));
}

QString channel_name_style() {
    const ThemeTokens& t = tokens();
    return QStringLiteral(
               "QLineEdit { background-color: %1; color: %2; border: 1px solid %3;"
               " border-radius: 8px; padding: 3px 6px; }")
        .arg(css(t.surface_low), css(t.ink_muted), css(t.border_soft));
}

QString short_media_name(const Clip& clip, const MediaEntry* media) {
    if (!clip.name.empty()) return QString::fromStdString(clip.name);
    if (media && !media->path.empty()) {
        const QString path = QString::fromStdString(media->path);
        return path.section(QLatin1Char('/'), -1);
    }
    return QStringLiteral("—");
}

QString hms_frames(const int64_t frame, const double fps) {
    return timecode(frame, fps);
}

struct FileControls {
    // Media preview header.
    QLabel* preview_icon = nullptr;
    QLabel* preview_name = nullptr;
    QToolButton* open_folder = nullptr;

    // Header Info (read-only).
    QLabel* r_media = nullptr;
    QLabel* r_path = nullptr;
    QLabel* r_video = nullptr;
    QLabel* r_fps = nullptr;
    QLabel* r_vstreams = nullptr;
    QLabel* r_astreams = nullptr;
    QLabel* r_source_tc = nullptr;
    QLabel* r_tc_rate = nullptr;

    // Metadata (editable).
    QLineEdit* ed_timecode = nullptr;
    QComboBox* ed_tag = nullptr;
    QLabel* tag_swatch = nullptr;
    QWidget* swatch_row = nullptr;
    std::vector<QToolButton*> swatches;
    QLineEdit* ed_name = nullptr;
    QPlainTextEdit* ed_notes = nullptr;
    QCheckBox* auto_select = nullptr;
    QCheckBox* next_box = nullptr;

    // Audio Configuration.
    QVBoxLayout* audio_body = nullptr;
    QLabel* audio_hint = nullptr;
    std::vector<QWidget*> channel_rows;

    // Timecode.
    QLineEdit* tc_current = nullptr;
    QLineEdit* tc_slate = nullptr;
    QLineEdit* tc_offset = nullptr;

    bool updating = false;   // guards against committing while populating
    bool attached = false;   // selection signals already connected
    std::optional<std::string> pending_name;
    std::optional<std::string> pending_comments;
    std::optional<uint8_t> pending_color;
};

std::map<MainWindow*, FileControls>& file_registry() {
    static std::map<MainWindow*, FileControls> reg;
    return reg;
}

FileControls* file_lookup(MainWindow& mw) {
    const auto it = file_registry().find(&mw);
    return it == file_registry().end() ? nullptr : &it->second;
}

QLineEdit* make_readonly_line(QWidget* parent) {
    auto* l = new QLineEdit(parent);
    l->setReadOnly(true);
    apply_theme_style(l, &readonly_line_style);
    return l;
}

QLineEdit* make_editable_line(QWidget* parent) {
    auto* l = new QLineEdit(parent);
    apply_theme_style(l, &editable_line_style);
    return l;
}

void set_dark_combo(QComboBox* cb, const QStringList& items) {
    cb->addItems(items);
    apply_theme_style(cb, &combo_style);
}

QLabel* make_readonly_label(QWidget* parent) {
    auto* l = new QLabel(parent);
    apply_theme_style(l, &muted_label_style);
    return l;
}

}  // namespace

void build_inspector_file(MainWindow& mw, QVBoxLayout* file_layout) {
    FileControls& fc = file_registry()[&mw];
    auto* host = file_layout->parentWidget();
    const auto tr = [](const char* s) { return MainWindow::tr(s); };

    // ---- Media preview header ----------------------------------------------
    // Rounded raised card that caps the page, matching the inspector's
    // semi-rounded category cards below it.
    auto* preview = new QWidget(host);
    preview->setObjectName(QStringLiteral("inspectorPreviewCard"));
    apply_theme_style(preview, &preview_card_style);
    auto* preview_layout = new QHBoxLayout(preview);
    preview_layout->setContentsMargins(12, 10, 10, 10);
    preview_layout->setSpacing(8);
    fc.preview_icon = new QLabel(preview);
    fc.preview_icon->setFixedSize(16, 16);
    fc.preview_icon->setPixmap(icon("app_icon").pixmap(16, 16));
    fc.preview_name = new QLabel(tr("Select a clip to inspect"), preview);
    apply_theme_style(fc.preview_name, &preview_name_style);
    fc.preview_name->setTextInteractionFlags(Qt::TextSelectableByMouse);
    fc.open_folder = new QToolButton(preview);
    fc.open_folder->setIcon(icon("viewport"));
    fc.open_folder->setIconSize(QSize(15, 15));
    fc.open_folder->setAutoRaise(true);
    fc.open_folder->setToolTip(tr("Locate source in file manager"));
    preview_layout->addWidget(fc.preview_icon);
    preview_layout->addWidget(fc.preview_name, 1);
    preview_layout->addWidget(fc.open_folder);
    file_layout->addWidget(preview);

    // ---- Header Info ---------------------------------------------------------
    auto* info = new InspectorCategory(tr("Header Info"), /*expanded=*/true, host);
    auto* info_body = info->body_layout();
    const auto add_ro_row = [&](const char* label, QLabel*& out) {
        out = make_readonly_label(host);
        auto* row = new QHBoxLayout;
        row->setSpacing(6);
        auto* lbl = new QLabel(MainWindow::tr(label));
        lbl->setMinimumWidth(88);
        apply_theme_style(lbl, &muted_label_style);
        row->addWidget(lbl);
        row->addWidget(out, 1);
        info_body->addLayout(row);
    };
    add_ro_row("Media", fc.r_media);
    add_ro_row("Path", fc.r_path);
    add_ro_row("Video Res", fc.r_video);
    add_ro_row("Frame Rate", fc.r_fps);
    add_ro_row("Video Streams", fc.r_vstreams);
    add_ro_row("Audio Streams", fc.r_astreams);
    add_ro_row("Source TC", fc.r_source_tc);
    add_ro_row("TC Rate", fc.r_tc_rate);
    file_layout->addWidget(info);

    // ---- Metadata ------------------------------------------------------------
    auto* meta = new InspectorCategory(tr("Metadata"), /*expanded=*/true, host);
    auto* meta_body = meta->body_layout();

    auto* tc_row = new QHBoxLayout;
    tc_row->setSpacing(6);
    auto* tc_lbl = new QLabel(MainWindow::tr("Timecode"));
    tc_lbl->setMinimumWidth(88);
    apply_theme_style(tc_lbl, &muted_label_style);
    fc.ed_timecode = make_editable_line(host);
    tc_row->addWidget(tc_lbl);
    tc_row->addWidget(fc.ed_timecode, 1);
    meta_body->addLayout(tc_row);

    auto* tag_row = new QHBoxLayout;
    tag_row->setSpacing(6);
    auto* tag_lbl = new QLabel(MainWindow::tr("Tag"));
    tag_lbl->setMinimumWidth(88);
    apply_theme_style(tag_lbl, &muted_label_style);
    fc.ed_tag = new QComboBox(host);
    set_dark_combo(fc.ed_tag, {tr("None"), tr("Good Take"), tr("Rejected")});
    fc.tag_swatch = new QLabel(host);
    fc.tag_swatch->setFixedSize(14, 14);
    fc.tag_swatch->setStyleSheet(
        QStringLiteral("background-color: transparent; border-radius: 7px;"));
    tag_row->addWidget(tag_lbl);
    tag_row->addWidget(fc.ed_tag, 1);
    tag_row->addWidget(fc.tag_swatch);
    meta_body->addLayout(tag_row);

    auto* color_lbl = new QLabel(MainWindow::tr("Colour"));
    color_lbl->setMinimumWidth(88);
    apply_theme_style(color_lbl, &muted_label_style);
    fc.swatch_row = new QWidget(host);
    auto* swatch_layout = new QHBoxLayout(fc.swatch_row);
    swatch_layout->setContentsMargins(0, 0, 0, 0);
    swatch_layout->setSpacing(3);
    auto* no_color = new QToolButton(fc.swatch_row);
    no_color->setText(QStringLiteral("\u2715"));
    no_color->setFixedSize(16, 16);
    no_color->setToolTip(tr("No colour"));
    apply_theme_style(no_color, &no_color_style);
    swatch_layout->addWidget(no_color);
    for (int i = 0; i < 12; ++i) {
        auto* sw = new QToolButton(fc.swatch_row);
        sw->setFixedSize(16, 16);
        sw->setToolTip(QStringLiteral("#%1").arg(clip_color_swatches()[i].name()));
        apply_theme_style(sw, [i] {
            return QStringLiteral(
                       "background-color: %1; border: 1px solid %2; border-radius: 4px;")
                .arg(clip_color_swatches()[i].name(), css(tokens().border_soft));
        });
        fc.swatches.push_back(sw);
        swatch_layout->addWidget(sw);
    }
    swatch_layout->addStretch(1);
    auto* color_row = new QHBoxLayout;
    color_row->setSpacing(6);
    color_row->addWidget(color_lbl);
    color_row->addWidget(fc.swatch_row, 1);
    meta_body->addLayout(color_row);

    auto* name_row = new QHBoxLayout;
    name_row->setSpacing(6);
    auto* name_lbl = new QLabel(MainWindow::tr("Clip Name"));
    name_lbl->setMinimumWidth(88);
    apply_theme_style(name_lbl, &muted_label_style);
    fc.ed_name = make_editable_line(host);
    name_row->addWidget(name_lbl);
    name_row->addWidget(fc.ed_name, 1);
    meta_body->addLayout(name_row);

    auto* notes_lbl = new QLabel(MainWindow::tr("Notes"));
    apply_theme_style(notes_lbl, &muted_label_style);
    meta_body->addWidget(notes_lbl);
    fc.ed_notes = new QPlainTextEdit(host);
    fc.ed_notes->setPlaceholderText(tr("Add notes about this clip..."));
    fc.ed_notes->setMaximumHeight(72);
    apply_theme_style(fc.ed_notes, &notes_style);
    meta_body->addWidget(fc.ed_notes);

    auto* checks_row = new QHBoxLayout;
    checks_row->setSpacing(18);
    fc.auto_select = new QCheckBox(tr("Auto Select"), host);
    fc.next_box = new QCheckBox(tr("Next"), host);
    for (QCheckBox* cb : {fc.auto_select, fc.next_box})
        apply_theme_style(cb, &muted_label_style);
    checks_row->addWidget(fc.auto_select);
    checks_row->addWidget(fc.next_box);
    checks_row->addStretch(1);
    meta_body->addLayout(checks_row);
    file_layout->addWidget(meta);

    // ---- Audio Configuration --------------------------------------------------
    auto* audio = new InspectorCategory(tr("Audio Configuration"), /*expanded=*/false, host);
    fc.audio_body = audio->body_layout();
    fc.audio_hint = new QLabel(
        tr("No audio channels for this source — drag an audio clip to the timeline first."),
        host);
    fc.audio_hint->setWordWrap(true);
    apply_theme_style(fc.audio_hint, &faint_label_style);
    fc.audio_body->addWidget(fc.audio_hint);
    file_layout->addWidget(audio);

    // ---- Timecode --------------------------------------------------------------
    auto* tc_groups = new InspectorCategory(tr("Timecode"), /*expanded=*/false, host);
    auto* tc_body = tc_groups->body_layout();
    fc.tc_current = make_readonly_line(host);
    fc.tc_slate = make_readonly_line(host);
    fc.tc_slate->setText(QStringLiteral("—"));
    fc.tc_offset = make_readonly_line(host);
    fc.tc_offset->setText(QStringLiteral("—"));
    const auto add_tc_row = [&](const char* label, QLineEdit* field) {
        auto* row = new QHBoxLayout;
        row->setSpacing(6);
        auto* lbl = new QLabel(MainWindow::tr(label));
        lbl->setMinimumWidth(88);
        apply_theme_style(lbl, &muted_label_style);
        row->addWidget(lbl);
        row->addWidget(field, 1);
        tc_body->addLayout(row);
    };
    add_tc_row("Current Timecode", fc.tc_current);
    add_tc_row("Slate", fc.tc_slate);
    add_tc_row("Offset", fc.tc_offset);
    file_layout->addWidget(tc_groups);

    // ---- Committing -------------------------------------------------------------
    const auto commit_metadata = [&mw, &fc]() {
        if (fc.updating) return;
        canvas::core::Track::Kind kind;
        std::size_t index;
        canvas::core::Clip clip;
        if (!mw.find_selected_clip(kind, index, clip)) return;

        const auto tag = static_cast<canvas::core::Clip::ClipTag>(fc.ed_tag->currentIndex());
        const uint8_t color = fc.pending_color.value_or(clip.clip_color);
        std::string name;
        if (fc.pending_name) name = *fc.pending_name;
        else if (!clip.name.empty()) name = clip.name;
        const std::string& comments =
            fc.pending_comments ? *fc.pending_comments : clip.comments;
        if (tag == clip.clip_tag && color == clip.clip_color && name == clip.name &&
            comments == clip.comments)
            return;

        auto cmd = canvas::core::set_clip_metadata(mw.project_->sequence, kind, index, clip.id,
                                                   tag, color, comments, name);
        if (!cmd) return;
        mw.undo_.record(std::move(cmd));
        mw.has_unsaved_changes_ = true;
        mw.push_snapshot();
        mw.refresh_timeline();
    };

    QObject::connect(fc.ed_tag, qOverload<int>(&QComboBox::currentIndexChanged), &mw,
                     commit_metadata);
    QObject::connect(fc.ed_name, &QLineEdit::editingFinished, &mw,
                     [&fc, commit_metadata]() {
                         fc.pending_name = fc.ed_name->text().toStdString();
                         commit_metadata();
                     });
    QObject::connect(fc.ed_notes, &QPlainTextEdit::textChanged, &mw, [&fc]() {
        if (!fc.updating) fc.pending_comments = fc.ed_notes->toPlainText().toStdString();
    });

    // Colour swatches commit immediately (single click = one undo step).
    const auto apply_swatch = [&mw, &fc, commit_metadata](uint8_t color) {
        fc.pending_color = color;
        const QColor c = clip_color_for(color);
        fc.tag_swatch->setStyleSheet(
            c.isValid() ? QStringLiteral("background-color: %1; border-radius: 7px;").arg(c.name())
                        : QStringLiteral("background-color: transparent; border-radius: 7px;"));
        commit_metadata();
    };
    for (int i = 0; i < 12; ++i) {
        QToolButton* sw = fc.swatches[static_cast<std::size_t>(i)];
QObject::connect(sw, &QToolButton::clicked, &mw,
                         [&fc, apply_swatch, &mw, i]() { apply_swatch(static_cast<uint8_t>(i + 1)); });
    }
    QObject::connect(no_color, &QToolButton::clicked, &mw,
                     [apply_swatch]() { apply_swatch(0); });

    // Timecode edits relocate the clip on its own track (in-place move).
    QObject::connect(fc.ed_timecode, &QLineEdit::editingFinished, &mw, [&mw, &fc]() {
        if (fc.updating) return;
        if (!mw.project_) return;
        const double fps = mw.project_->sequence.fps;
        const QString& text = fc.ed_timecode->text().trimmed();
        const QRegularExpression re(QStringLiteral(
            R"(^\s*(\d+):(\d\d):(\d\d):(\d\d)\s*$)"));
        const QRegularExpressionMatch m = re.match(text);
        const int64_t frame =
            m.hasMatch()
                ? (m.captured(1).toLongLong() * 3600 + m.captured(2).toLongLong() * 60 +
                   m.captured(3).toLongLong()) * static_cast<int64_t>(std::lround(fps)) +
                      m.captured(4).toLongLong()
                : text.toLongLong();
        if (frame < 0) return;
        canvas::core::Track::Kind kind;
        std::size_t index;
        canvas::core::Clip clip;
        if (!mw.find_selected_clip(kind, index, clip)) return;
        if (frame == clip.tl_in) return;
        auto cmd = canvas::core::move_clip(mw.project_->sequence, kind, index, clip.id, kind,
                                           index, frame);
        if (!cmd) return;
        mw.undo_.record(std::move(cmd));
        mw.has_unsaved_changes_ = true;
        mw.push_snapshot();
        mw.refresh_timeline();
        fc.ed_timecode->setText(hms_frames(frame, fps));
    });

    // Open the source file's folder with the system file manager.
    QObject::connect(fc.open_folder, &QToolButton::clicked, &mw, [&mw, &fc]() {
        if (!mw.project_) return;
        canvas::core::Track::Kind kind;
        std::size_t index;
        canvas::core::Clip clip;
        if (!mw.find_selected_clip(kind, index, clip)) return;
        const auto* media = mw.project_->media_by_id(clip.media);
        if (!media || media->path.empty()) return;
        const QString path = QString::fromStdString(media->path);
        const QString folder = path.contains(QLatin1Char('/'))
                                   ? path.section(QLatin1Char('/'), 0, -2)
                                   : QStringLiteral("/");
        QDesktopServices::openUrl(QUrl::fromLocalFile(folder));
    });
}

void attach_inspector_file(MainWindow& mw, TimelineWidget* timeline) {
    FileControls* fc = file_lookup(mw);
    if (!fc || !timeline || fc->attached) return;
    fc->attached = true;
    QObject::connect(timeline, &TimelineWidget::clip_selected, &mw,
                     [&mw](const canvas::core::Clip*) { update_inspector_file(mw); });
    QObject::connect(timeline, &TimelineWidget::clips_range_selected, &mw,
                     [&mw](std::vector<canvas::core::ClipId>) { update_inspector_file(mw); });
}

void update_inspector_file(MainWindow& mw) {
    FileControls* fc = file_lookup(mw);
    if (!fc || !mw.project_) return;

    canvas::core::Track::Kind kind;
    std::size_t index;
    canvas::core::Clip clip;
    const bool has_clip = mw.find_selected_clip(kind, index, clip);
    const bool enabled = has_clip;

    QWidget* enable_set[] = {fc->preview_name, fc->r_media, fc->r_path, fc->r_video, fc->r_fps,
                         fc->r_vstreams, fc->r_astreams, fc->r_source_tc, fc->r_tc_rate,
                         fc->ed_timecode, fc->ed_tag, fc->swatch_row, fc->ed_name,
                         fc->ed_notes, fc->tc_current, fc->tc_slate, fc->tc_offset};
    for (QWidget* w : enable_set)
        if (w) w->setEnabled(enabled);
    fc->open_folder->setEnabled(enabled);
    fc->auto_select->setEnabled(false);
    fc->next_box->setEnabled(false);
    if (!enabled) {
        fc->preview_name->setText(MainWindow::tr("Select a clip to inspect"));
        if (fc->audio_hint) fc->audio_hint->setVisible(true);
        for (QWidget* w : fc->channel_rows) w->setVisible(false);
        return;
    }

    const auto* media = mw.project_->media_by_id(clip.media);
    const double fps = mw.project_->sequence.fps;

    fc->preview_name->setText(short_media_name(clip, media));
    fc->r_media->setText(media ? QString::fromStdString(media->path).section(QLatin1Char('/'), -1)
                              : QStringLiteral("—"));
    fc->r_path->setText(media ? QString::fromStdString(media->path) : QStringLiteral("—"));
    fc->r_video->setText(media && media->width > 0 && media->height > 0
                            ? QStringLiteral("%1 \u00D7 %2")
                                  .arg(media->width)
                                  .arg(media->height)
                            : QStringLiteral("—"));
    fc->r_fps->setText(media && media->fps > 0.0
                          ? QStringLiteral("%1 fps (sequence %2)")
                                .arg(media->fps, 0, 'f', 2)
                                .arg(fps, 0, 'f', 2)
                          : QStringLiteral("%1 fps").arg(fps, 0, 'f', 2));
    fc->r_vstreams->setText(media && media->width > 0 && media->height > 0 ? QStringLiteral("1")
                                                                          : QStringLiteral("0"));
    const bool has_audio = kind == Track::Kind::Audio ||
                           (clip.linked_id != 0 &&
                            [&mw](canvas::core::ClipId id) {
                                for (const auto& t : mw.project_->sequence.audio_tracks)
                                    if (t.clip_with_id(id)) return true;
                                return false;
                            }(clip.linked_id));
    fc->r_astreams->setText(has_audio ? QStringLiteral("1") : QStringLiteral("0"));
    fc->r_source_tc->setText(media ? hms_frames(clip.src_in, fps) : QStringLiteral("—"));
    fc->r_tc_rate->setText(QStringLiteral("%1 fps").arg(fps, 0, 'f', 2));

    fc->updating = true;
    fc->ed_timecode->setText(hms_frames(clip.tl_in, fps));
    fc->ed_tag->setCurrentIndex(static_cast<int>(clip.clip_tag));
    fc->tag_swatch->setStyleSheet(
        clip_color_for(clip.clip_color).isValid()
            ? QStringLiteral("background-color: %1; border-radius: 7px;")
                  .arg(clip_color_for(clip.clip_color).name())
            : QStringLiteral("background-color: transparent; border-radius: 7px;"));
    fc->pending_color.reset();
    fc->ed_name->setText(clip.name.empty() ? QString() : QString::fromStdString(clip.name));
    fc->ed_notes->setPlainText(QString::fromStdString(clip.comments));
    fc->pending_name.reset();
    fc->pending_comments.reset();
    fc->updating = false;

    fc->tc_current->setText(hms_frames(clip.tl_in, fps));

    // Audio configuration rows: rebuild per selected clip.
    const int channels = has_audio ? 1 : 0;
    for (QWidget* w : fc->channel_rows) {
        w->setVisible(false);
        w->deleteLater();
    }
    fc->channel_rows.clear();
    for (int ch = 0; ch < channels; ++ch) {
        auto* row = new QWidget(fc->audio_body->widget());
        auto* lay = new QHBoxLayout(row);
        lay->setContentsMargins(0, 0, 0, 0);
        lay->setSpacing(6);
        auto* play = new QToolButton(row);
        play->setIcon(icon("play"));
        play->setIconSize(QSize(14, 14));
        play->setAutoRaise(true);
        play->setToolTip(
            MainWindow::tr("Audition this channel — not connected in this build"));
        play->setEnabled(false);
        auto* level = new QProgressBar(row);
        level->setRange(0, 100);
        level->setValue(0);
        level->setTextVisible(false);
        level->setFixedHeight(6);
        apply_theme_style(level, &progress_style);
        auto* ch_name = new QLineEdit(row);
        ch_name->setText(QStringLiteral("Channel %1").arg(ch + 1));
        apply_theme_style(ch_name, &channel_name_style);
        lay->addWidget(play);
        lay->addWidget(level, 1);
        lay->addWidget(ch_name, 2);
        fc->audio_body->addWidget(row);
        fc->channel_rows.push_back(row);
    }
    if (fc->audio_hint) fc->audio_hint->setVisible(channels == 0);
}

void apply_inspector_file(MainWindow& mw) {
    (void)mw;  // commits are event-driven from the control signals
}

}  // namespace canvas::gui