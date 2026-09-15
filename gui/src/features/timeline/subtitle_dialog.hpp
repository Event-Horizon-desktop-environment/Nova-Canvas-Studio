#pragma once

// "Generate Subtitles From Audio" popup (Timeline > AI Tools). Pure
// presentation: collects the language + caption-style policy and hands it to
// the caller as one SubtitlePolicy. While a transcription worker runs, the
// caller drives the dialog with set_running()/set_progress() so the form is
// locked behind a live progress bar, a stage line, and a performance readout;
// generateRequested() fires once per Generate press (the caller decides when
// the run is done). Cancel (or Esc/X) mid-run emits cancelRequested() and
// leaves the dialog open — the caller's finish path re-arms it.

#include <QDialog>
#include <QString>

#include "canvas/core/timeline/captions.hpp"

class QComboBox;
class QLabel;
class QProgressBar;
class QPushButton;
class QSpinBox;

namespace canvas::gui {

struct SubtitlePolicy {
    QString language;  // whisper language code; empty = auto-detect
    canvas::core::captions::Options options;
};

class SubtitleDialog : public QDialog {
    Q_OBJECT
   public:
    explicit SubtitleDialog(QWidget* parent = nullptr);

    // The policy currently shown in the form (valid before the worker starts).
    SubtitlePolicy policy() const;

    // Locks the form and shows `status` under the controls while a worker is
    // in flight; Generate becomes the busy caption. A running dialog cannot be
    // accepted (the caller calls set_running(false) then accept(), or
    // set_done() for the success summary).
    void set_running(bool running, const QString& status = {});
    // Replaces the status line without touching the running lock or the form
    // (used to surface worker errors/outcomes while the dialog stays open).
    void set_status(const QString& status);
    // Live progress tick from the caller's poll loop (GUI thread only).
    void set_progress(int percent, const QString& stage, const QString& perf);
    // Success terminal: keeps the 100% bar + `summary` visible and swaps
    // Generate for a Done button that accepts the dialog.
    void set_done(const QString& summary);
    // Cancel-pressed terminal: the abort request is in flight; the Cancel
    // button shows it and stops accepting presses until finish() re-arms.
    void set_cancelling();

    void reject() override;

   signals:
    // Emitted when Generate is pressed; carries the policy to run against.
    void generateRequested(const canvas::gui::SubtitlePolicy& policy);
    // Emitted when Cancel/Esc/X is pressed mid-run; the caller should flag the
    // engine's Progress::cancel and let its finish path re-arm this dialog.
    void cancelRequested();

   private:
    void apply_preset(int index);
    void note_custom_change();

    QComboBox* language_ = nullptr;
    QComboBox* preset_ = nullptr;
    QSpinBox* chars_ = nullptr;
    QComboBox* lines_ = nullptr;
    QSpinBox* gap_ = nullptr;
    QPushButton* generate_btn_ = nullptr;
    QPushButton* cancel_btn_ = nullptr;
    QProgressBar* progress_ = nullptr;
    QLabel* status_ = nullptr;
    QLabel* perf_ = nullptr;
    bool lock_preset_ = false;
    bool running_ = false;
    bool done_ = false;
};

}  // namespace canvas::gui