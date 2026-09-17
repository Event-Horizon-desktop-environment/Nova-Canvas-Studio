#include "features/timeline/subtitle_dialog.hpp"

#include "UX/theme.hpp"
#include "canvas/core/timeline/captions.hpp"
#include "canvas/core/media/transcript.hpp"

#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QFrame>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QProgressBar>
#include <QPushButton>
#include <QSpinBox>
#include <QVBoxLayout>

#include <algorithm>
#include <string>

namespace canvas::gui {

namespace {

const char* preset_label(const char* slug) {
    if (std::string_view(slug) == "standard") return "Standard";
    if (std::string_view(slug) == "classic") return "Classic Subtitle";
    if (std::string_view(slug) == "social") return "Social Caption";
    if (std::string_view(slug) == "burned") return "Burned Caption";
    return slug;
}

struct LangEntry {
    const char* code;
    const char* label;
};
const LangEntry kLanguages[] = {
    {"", "Auto Detect"},
    {"en", "English"},
    {"zh", "Chinese"},
    {"es", "Spanish"},
    {"fr", "French"},
    {"de", "German"},
    {"it", "Italian"},
    {"pt", "Portuguese"},
    {"ru", "Russian"},
    {"ja", "Japanese"},
    {"ko", "Korean"},
    {"ar", "Arabic"},
    {"hi", "Hindi"},
    {"tr", "Turkish"},
    {"nl", "Dutch"},
    {"pl", "Polish"},
};

}

SubtitleDialog::SubtitleDialog(QWidget* parent) : QDialog(parent) {
    setWindowTitle(tr("Generate Subtitles From Audio"));
    setModal(true);
    setMinimumWidth(520);
    resize(560, 0);

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(18, 18, 18, 14);
    root->setSpacing(14);

    auto* intro = new QLabel(
        tr("Creates an on-timeline caption bar per spoken phrase from the "
           "selected audio clip, then writes an SRT alongside the source file.\n"
           "Transcription runs locally with whisper.cpp — no audio leaves this "
           "machine."),
        this);
    intro->setWordWrap(true);
    intro->setStyleSheet(QStringLiteral("color: %1; font-size: 12px;")
                             .arg(css(canvas::gui::tokens().ink_muted)));
    root->addWidget(intro);

    QGroupBox* language_box = new QGroupBox(tr("Language"), this);
    {
        auto* lang_form = new QFormLayout(language_box);
        lang_form->setContentsMargins(12, 16, 12, 12);
        lang_form->setHorizontalSpacing(14);
        language_ = new QComboBox(language_box);
        for (const LangEntry& lang : kLanguages) {
            language_->addItem(tr(lang.label), QString::fromLatin1(lang.code));
        }
        language_->setCurrentIndex(0);
        lang_form->addRow(tr("Spoken language"), language_);
    }
    root->addWidget(language_box);

    QGroupBox* style_box = new QGroupBox(tr("Caption Preset"), this);
    {
        auto* style_form = new QFormLayout(style_box);
        style_form->setContentsMargins(12, 16, 12, 12);
        style_form->setHorizontalSpacing(14);
        style_form->setVerticalSpacing(10);

        preset_ = new QComboBox(style_box);
        for (const auto& p : canvas::core::captions::presets()) {
            preset_->addItem(tr(preset_label(p.name)), QString::fromLatin1(p.name));
        }
        preset_->addItem(tr("Custom"), QStringLiteral("custom"));
        style_form->addRow(tr("Style"), preset_);

        chars_ = new QSpinBox(style_box);
        chars_->setRange(4, 160);
        chars_->setValue(42);
        chars_->setSuffix(tr(" chars"));
        style_form->addRow(tr("Max chars per line"), chars_);

        lines_ = new QComboBox(style_box);
        lines_->addItem(tr("Single"), 1);
        lines_->addItem(tr("Double"), 2);
        lines_->addItem(tr("Triple"), 3);
        style_form->addRow(tr("Lines per caption"), lines_);

        gap_ = new QSpinBox(style_box);
        gap_->setRange(0, 30);
        gap_->setValue(0);
        gap_->setSuffix(tr(" frames"));
        style_form->addRow(tr("Gap between subtitles"), gap_);
    }
    root->addWidget(style_box);

    status_ = new QLabel(this);
    status_->setWordWrap(true);
    status_->hide();
    root->addWidget(status_);

    progress_ = new QProgressBar(this);
    progress_->setRange(0, 100);
    progress_->setValue(0);
    progress_->setTextVisible(true);
    progress_->hide();
    root->addWidget(progress_);

    perf_ = new QLabel(this);
    perf_->setWordWrap(true);
    perf_->setStyleSheet(QStringLiteral("color: %1; font-size: 12px;")
                             .arg(css(canvas::gui::tokens().ink_muted)));
    perf_->hide();
    root->addWidget(perf_);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Cancel, this);
    cancel_btn_ = buttons->button(QDialogButtonBox::Cancel);
    generate_btn_ = buttons->addButton(tr("Generate Subtitles"), QDialogButtonBox::AcceptRole);
    connect(buttons, &QDialogButtonBox::rejected, this, &SubtitleDialog::reject);
    connect(generate_btn_, &QPushButton::clicked, this, [this] {
        if (done_) {
            accept();
            return;
        }
        if (running_) return;
        emit generateRequested(policy());
    });
    root->addWidget(buttons);

    connect(preset_, &QComboBox::currentIndexChanged, this, &SubtitleDialog::apply_preset);
    connect(chars_, &QSpinBox::valueChanged, this, &SubtitleDialog::note_custom_change);
    connect(lines_, &QComboBox::currentIndexChanged, this, &SubtitleDialog::note_custom_change);
    connect(gap_, &QSpinBox::valueChanged, this, &SubtitleDialog::note_custom_change);

    apply_preset(0);
}

SubtitlePolicy SubtitleDialog::policy() const {
    SubtitlePolicy p;
    p.language = language_->currentData().toString();
    p.options.max_chars_per_line = chars_->value();
    p.options.max_lines = lines_->currentData().toInt();
    p.options.gap_frames = gap_->value();
    return p;
}

void SubtitleDialog::set_running(bool running, const QString& status) {
    running_ = running;
    done_ = false;
    language_->setEnabled(!running);
    preset_->setEnabled(!running);
    chars_->setEnabled(!running);
    lines_->setEnabled(!running);
    gap_->setEnabled(!running);
    generate_btn_->setEnabled(!running);
    if (cancel_btn_) cancel_btn_->setEnabled(true);
    if (running) {
        generate_btn_->setText(tr("Transcribing…"));
        if (cancel_btn_) cancel_btn_->setText(tr("Cancel"));
        status_->setText(status.isEmpty() ? tr("Transcribing audio…") : status);
        status_->show();
        progress_->setValue(0);
        progress_->show();
        perf_->setText(tr("Warming up…"));
        perf_->show();
    } else {
        generate_btn_->setText(tr("Generate Subtitles"));
        status_->hide();
        progress_->hide();
        perf_->hide();
    }
}

void SubtitleDialog::set_progress(int percent, const QString& stage, const QString& perf) {
    if (!running_) return;
    progress_->setValue(std::clamp(percent, 0, 100));
    status_->setText(stage);
    status_->show();
    perf_->setText(perf);
    perf_->show();
}

void SubtitleDialog::set_done(const QString& summary) {
    running_ = false;
    done_ = true;
    progress_->setValue(100);
    progress_->show();
    status_->setText(summary);
    status_->show();
    perf_->hide();
    generate_btn_->setText(tr("Done"));
    generate_btn_->setEnabled(true);
    if (cancel_btn_) cancel_btn_->setEnabled(true);
}

void SubtitleDialog::set_cancelling() {
    if (!running_) return;
    if (cancel_btn_) {
        cancel_btn_->setText(tr("Cancelling…"));
        cancel_btn_->setEnabled(false);
    }
    status_->setText(tr("Cancelling — stopping the transcription engine…"));
    status_->show();
}

void SubtitleDialog::reject() {
    if (running_) {
        emit cancelRequested();
        return;
    }
    QDialog::reject();
}

void SubtitleDialog::set_status(const QString& status) {
    status_->setText(status.isEmpty() ? tr("…") : status);
    status_->show();
}

void SubtitleDialog::apply_preset(int index) {
    if (index < 0) return;
    const QString slug = preset_->itemData(index).toString();
    if (slug == QStringLiteral("custom")) return;
    const auto& opts = canvas::core::captions::preset_options(slug.toStdString());
    lock_preset_ = true;
    chars_->setValue(opts.max_chars_per_line);
    lines_->setCurrentIndex(std::clamp(opts.max_lines - 1, 0, 2));
    gap_->setValue(static_cast<int>(opts.gap_frames));
    lock_preset_ = false;
}

void SubtitleDialog::note_custom_change() {
    if (lock_preset_ || running_) return;
    const int custom_index = preset_->count() - 1;
    if (preset_->currentIndex() != custom_index) preset_->setCurrentIndex(custom_index);
}

}
