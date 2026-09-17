#pragma once

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
    QString language;
    canvas::core::captions::Options options;
};

class SubtitleDialog : public QDialog {
    Q_OBJECT
   public:
    explicit SubtitleDialog(QWidget* parent = nullptr);

    SubtitlePolicy policy() const;

    void set_running(bool running, const QString& status = {});
    void set_status(const QString& status);
    void set_progress(int percent, const QString& stage, const QString& perf);
    void set_done(const QString& summary);
    void set_cancelling();

    void reject() override;

   signals:
    void generateRequested(const canvas::gui::SubtitlePolicy& policy);
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

}
