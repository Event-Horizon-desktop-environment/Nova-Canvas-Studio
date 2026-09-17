#include "UX/MainWindow.hpp"
#include "UX/theme.hpp"
#include "Logging.hpp"

#include "features/timeline/audio_targets.hpp"
#include "features/timeline/subtitle_dialog.hpp"

#include <QDir>
#include <QFileInfo>
#include <QMessageBox>
#include <QPointer>
#include <QStatusBar>
#include <QTimer>

#include <algorithm>
#include <cmath>
#include <string>
#include <thread>

#include "canvas/core/media/transcribe.hpp"
#include "canvas/core/timeline/captions.hpp"
#include "canvas/core/timeline/title.hpp"

namespace canvas::gui {

namespace {
using canvas::core::captions::Caption;
using canvas::core::captions::Options;
using canvas::core::transcribe::Progress;
using canvas::core::transcribe::Report;
using canvas::core::transcribe::Result;

QString stage_text(const Progress::Stage stage) {
    switch (stage) {
        case Progress::Stage::kDecodingAudio:
            return SubtitleDialog::tr("Decoding the audio stream…");
        case Progress::Stage::kLoadingModel:
            return SubtitleDialog::tr("Loading the whisper model…");
        case Progress::Stage::kTranscribing:
            return SubtitleDialog::tr("Transcribing audio…");
        case Progress::Stage::kFinished:
            return SubtitleDialog::tr("Finishing…");
    }
    return {};
}

std::size_t cue_words(const std::vector<canvas::core::transcript::Segment>& cues) {
    std::size_t words = 0;
    for (const auto& cue : cues) {
        bool in_word = false;
        for (const char ch : cue.text) {
            const bool space = ch == ' ' || ch == '\t' || ch == '\n';
            if (!space && !in_word) {
                ++words;
                in_word = true;
            } else if (space) {
                in_word = false;
            }
        }
    }
    return words;
}
}

void MainWindow::open_subtitle_dialog() {
    if (!project_) return;
    if (subtitle_busy_.load()) {
        status_->showMessage(tr("Subtitles are already being generated…"), 3000);
        return;
    }

    const auto targets = resolve_audio_targets(project_->sequence, selected_clip_ids_);
    if (targets.empty()) {
        QMessageBox::information(
            this, tr("Generate Subtitles From Audio"),
            tr("Select an audio clip on the timeline first. Subtitles are "
               "generated from the audio selection's clip media."));
        return;
    }

    const auto& media = project_->media;
    const canvas::core::Clip audio_clip = targets[0].clip;
    if (audio_clip.media < 0 || audio_clip.media >= static_cast<int>(media.size())) {
        QMessageBox::information(this, tr("Generate Subtitles From Audio"),
                                 tr("The selected clip has no attached media."));
        return;
    }
    const canvas::core::MediaEntry& entry = media[static_cast<std::size_t>(audio_clip.media)];

    if (canvas::core::transcribe::resolve_model_path().empty()) {
        QMessageBox::warning(
            this, tr("Whisper Model Not Found"),
            tr("Transcription needs a whisper.cpp model file. Put one of the "
               "ggml-{base,small,medium}.bin models at:\n\n  %1/ggml-base.bin\n\n"
               "(or set $CANVAS_WHISPER_MODEL to the model's path). The model is "
               "a ~140 MB download from the whisper.cpp releases.")
                .arg(QString::fromStdString(
                    std::string("~/.cache/nova-canvas/whisper"))));
        return;
    }

    subtitle_media_path_ = QString::fromStdString(entry.path);
    subtitle_media_id_ = audio_clip.media;
    subtitle_tl_in_ = std::max<int64_t>(0, audio_clip.tl_in);
    subtitle_tl_out_ = std::max<int64_t>(subtitle_tl_in_, audio_clip.tl_out);
    subtitle_src_in_ = std::max<int64_t>(0, audio_clip.src_in);
    subtitle_seq_fps_ = project_->sequence.fps > 0.0 ? project_->sequence.fps : 30.0;
    subtitle_media_fps_ = entry.fps > 0.0 ? entry.fps : subtitle_seq_fps_;

    auto* dlg = new SubtitleDialog(this);
    subtitle_dialog_ = dlg;
    auto* poll = new QTimer(dlg);
    QObject::connect(poll, &QTimer::timeout, this, [this] {
        if (!subtitle_dialog_ || !subtitle_progress_) return;
        const Progress* p = subtitle_progress_.get();
        const QString perf =
            QStringLiteral("%1 · %2 s elapsed")
                .arg(QString::fromStdString(subtitle_model_name_))
                .arg(subtitle_elapsed_.elapsed() / 1000.0, 0, 'f', 1);
        subtitle_dialog_->set_progress(p->percent.load(std::memory_order_relaxed),
                                       stage_text(p->stage.load(std::memory_order_relaxed)),
                                       perf);
    });
    QObject::connect(dlg, &SubtitleDialog::cancelRequested, this, [this] {
        if (subtitle_progress_) subtitle_progress_->cancel.store(true);
        if (subtitle_dialog_) subtitle_dialog_->set_cancelling();
    });
    QObject::connect(dlg, &SubtitleDialog::generateRequested, this,
                     [this](const SubtitlePolicy& p) {
        if (!subtitle_dialog_ || subtitle_busy_.load()) return;
        subtitle_language_ = p.language.toStdString();
        subtitle_opts_ = p.options;
        subtitle_busy_.store(true);
        subtitle_progress_ = std::make_shared<Progress>();
        subtitle_elapsed_.restart();
        subtitle_model_name_ =
            QFileInfo(QString::fromStdString(canvas::core::transcribe::resolve_model_path()))
                .fileName()
                .toStdString();
        subtitle_dialog_->set_running(
            true, tr("Loading the model and transcribing audio — this can take "
                     "a while for long clips…"));
        const QString path = subtitle_media_path_;
        const std::string lang = p.language.toStdString();
        const std::shared_ptr<Progress> prog = subtitle_progress_;
        subtitle_worker_ = std::thread([this, path, lang, prog] {
            Report r;
            const std::string model = canvas::core::transcribe::resolve_model_path();
            if (model.empty()) {
                r.result = Result::kNoModel;
                r.error = "No whisper model file installed.";
            } else {
                canvas::core::transcribe::Options o;
                o.threads = 4;
                o.max_len = 0;
                o.max_tokens = 0;
                o.language = lang.empty() ? nullptr : lang.c_str();
                o.verbose = false;
                o.progress = prog.get();
                r = canvas::core::transcribe::transcribe_file(path.toStdString(), model, o);
            }
            QMetaObject::invokeMethod(
                this, [this, r] { finish_subtitle_transcription(r); }, Qt::QueuedConnection);
        });
    });
    poll->start(120);
    dlg->exec();
    if (subtitle_worker_.joinable()) subtitle_worker_.join();
    subtitle_dialog_.clear();
    subtitle_progress_.reset();
}

void MainWindow::finish_subtitle_transcription(Report report) {
    subtitle_busy_.store(false);
    subtitle_progress_.reset();
    const bool dialog_alive = (subtitle_dialog_ != nullptr);
    if (dialog_alive) subtitle_dialog_->set_running(false);
    if (!project_) return;

    const auto fail = [&](const QString& message) {
        if (dialog_alive) {
            subtitle_dialog_->set_status(message);
        } else {
            QMessageBox::warning(this, tr("Generate Subtitles From Audio"), message);
        }
    };

    if (report.result == Result::kAborted) {
        const QString msg = tr("Cancelled — no subtitles placed.");
        if (dialog_alive) {
            subtitle_dialog_->set_status(msg);
        } else {
            status_->showMessage(msg, 5000);
        }
        return;
    }

    if (report.result != Result::kDone) {
        QString why;
        switch (report.result) {
            case Result::kNoModel:
                why = tr("No whisper model file is installed.");
                break;
            case Result::kBadInput:
                why = tr("No decodable audio was found in the media.");
                break;
            case Result::kCouldNotInit:
                why = tr("The whisper model could not be loaded.");
                break;
            case Result::kRunFailed:
                why = tr("The transcription engine failed while decoding.");
                break;
            default:
                why.clear();
        }
        if (!report.error.empty() && report.result != Result::kNoModel)
            why = why.isEmpty() ? QString::fromStdString(report.error)
                                : why + QStringLiteral("\n\n") +
                                      QString::fromStdString(report.error);
        fail(why);
        return;
    }

    auto& seq = project_->sequence;
    const auto caps = canvas::core::captions::shape_captions(
        report.cues, subtitle_opts_, subtitle_seq_fps_);
    if (caps.empty()) {
        fail(tr("Transcription finished but no speech was detected in the "
                "selected audio."));
        return;
    }

    const QFileInfo finfo(subtitle_media_path_);
    const QString srt_path = finfo.dir().filePath(finfo.completeBaseName() + QStringLiteral(".srt"));
    const bool wrote_srt =
        canvas::core::transcript::write_srt(srt_path.toStdString(), report.cues);

    std::size_t top = seq.video_tracks.size();
    if (top > 0) {
        --top;
        if (!seq.video_tracks[top].clips.empty()) top = seq.video_tracks.size();
    }
    ensure_tracks_at(canvas::core::Track::Kind::Video, top);

    const double seq_fps = subtitle_seq_fps_;
    const double media_fps = subtitle_media_fps_;
    const int64_t body_first = subtitle_tl_in_;
    const int64_t body_last = std::max(body_first, subtitle_tl_out_ - 1);
    std::size_t placed = 0;
    for (const Caption& cap : caps) {
        const double s = static_cast<double>(cap.start_ms) / 1000.0;
        const double e = static_cast<double>(cap.end_ms) / 1000.0;
        int64_t w_s = body_first +
                      static_cast<int64_t>(std::llround(
                          (s - subtitle_src_in_ / media_fps) * seq_fps));
        int64_t w_e = body_first +
                      static_cast<int64_t>(std::llround(
                          (e - subtitle_src_in_ / media_fps) * seq_fps));
        w_s = std::clamp(w_s, body_first, body_last);
        w_e = std::clamp(std::max(w_e, w_s), body_first, body_last);
        const int64_t dur = std::max<int64_t>(1, w_e - w_s + 1);

        canvas::core::Clip clip;
        clip.media = -1;
        clip.tl_in = w_s;
        clip.src_in = 0;
        clip.src_out = dur;
        clip.name = "Caption";
        clip.title.text = cap.text;
        int frame_w = 1920;
        int frame_h = 1080;
        if (const auto* me = project_->media_by_id(subtitle_media_id_);
            me && me->width > 0 && me->height > 0) {
            frame_w = me->width;
            frame_h = me->height;
        }
        const auto fit = canvas::core::title::fit_caption(
            cap.text, frame_w, frame_h,
            canvas::core::title::find_font_path_for(clip.title.font_family));
        clip.title.size = fit.size;
        clip.pos_y = fit.pos_y;

        auto cmd = canvas::core::place_clip(seq, canvas::core::Track::Kind::Video, top,
                                            std::move(clip),
                                            canvas::core::Placement::Overwrite, 0.0);
        if (!cmd) continue;
        undo_.record(std::move(cmd));
        ++placed;
    }

    if (placed > 0) {
        has_unsaved_changes_ = true;
        push_snapshot();
        refresh_timeline();
    }

    const std::size_t words = cue_words(report.cues);
    const double audio_s = report.audio_seconds;
    const double wall_s = report.wall_seconds;
    const QString speed = wall_s > 0.01 ? QStringLiteral("%1× realtime").arg(audio_s / wall_s,
                                                                              0, 'f', 1)
                                        : QStringLiteral("—");
    const QString summary =
        tr("%1 caption %2 placed from %3 cues (%4 words).\n"
           "%5 of audio transcribed in %6 s (%7)%8")
            .arg(placed)
            .arg(placed == 1 ? tr("bar") : tr("bars"))
            .arg(report.cues.size())
            .arg(words)
            .arg(audio_s, 0, 'f', 1)
            .arg(wall_s, 0, 'f', 1)
            .arg(speed)
            .arg(wrote_srt ? tr(".\nSRT written to %1")
                                 .arg(QDir::toNativeSeparators(srt_path))
                           : tr(".\n(SRT sidecar write failed)"));
    if (dialog_alive) {
        status_->showMessage(
            tr("%1 caption %2 placed (%3)").arg(placed).arg(placed == 1 ? tr("bar") : tr("bars")).arg(speed),
            8000);
        subtitle_dialog_->set_done(summary);
    } else {
        QMessageBox::information(
            this, tr("Generate Subtitles From Audio"),
            placed > 0 ? summary : tr("No caption bars could be placed."));
    }
}

}
