#include "features/deliver/render_queue_panel.hpp"

#include "UX/theme.hpp"

#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QListView>
#include <QPainter>
#include <QPixmap>
#include <QProgressBar>
#include <QPushButton>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

namespace canvas::gui {

namespace {

// "HH:MM:SS" from a seconds count (the status field's formatting in both the
// time-remaining and completed modes).
QString format_duration(double secs) {
    int s = std::max(0, (int)std::llround(secs));
    const int h = s / 3600;
    s %= 3600;
    const int m = s / 60;
    s %= 60;
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%02d:%02d:%02d", h, m, s);
    return QString::fromLatin1(buf);
}

QString grip_pixmap_color() {
    return css(tokens().ink_faint);
}

QString rq_list_style() {
    const ThemeTokens& t = tokens();
    return QStringLiteral(
               "QListWidget{background:%1;border:1px solid %2;border-radius:8px;color:%3;}"
               "QListWidget::item{border-radius:8px;margin:2px 3px;background:transparent;}"
               "QListWidget::item:selected{background:%4;}")
        .arg(css(t.surface_low), css(t.border_soft), css(t.ink), css(t.accent_soft));
}

QString rq_progress_style() {
    const ThemeTokens& t = tokens();
    return QStringLiteral(
               "QProgressBar{background:%1;border:none;border-radius:3px;}"
               "QProgressBar::chunk{background:%2;border-radius:3px;}")
        .arg(css(t.surface_low), css(t.accent));
}

QString rq_render_btn_style() {
    const ThemeTokens& t = tokens();
    return QStringLiteral(
               "QPushButton{background:%1;color:%2;"
               "border-radius:8px;padding:7px 14px;font-weight:600;}"
               "QPushButton:hover{background:%3;}")
        .arg(css(t.accent), css(t.on_accent), css(t.accent_hover));
}

QString rq_cancel_btn_style() {
    const ThemeTokens& t = tokens();
    return QStringLiteral(
               "QPushButton{background:%1;color:%2;border:1px solid %3;"
               "border-radius:8px;padding:7px 14px;}"
               "QPushButton:hover{background:%4;color:%5;}")
        .arg(css(t.danger_soft), css(t.danger), css(t.danger),
             css(t.danger), css(t.on_accent));
}

QString rq_clear_btn_style() {
    const ThemeTokens& t = tokens();
    return QStringLiteral(
               "QPushButton{background:%1;color:%2;"
               "border:1px solid %3;border-radius:8px;padding:7px 14px;}"
               "QPushButton:hover{background:%4;border-color:%5;}")
        .arg(css(t.surface_raised), css(t.ink_muted), css(t.border_soft),
             css(t.surface_higher), css(t.border));
}

QString rq_row_action_style() {
    return QStringLiteral(
               "QToolButton{background:transparent;border:none;border-radius:5px;}"
               "QToolButton:hover{background:%1;}")
        .arg(css(tokens().state_hover));
}

// Single status field, reused per job state: a live time-remaining estimate
// (plus render speed) while rendering, total elapsed time once settled.
QString status_text(const canvas::core::RenderJob& j) {
    using S = canvas::core::RenderJob::Status;
    switch (j.status) {
        case S::Queued: return QStringLiteral("Queued");
        case S::Completed:
            // Finished cards read like Resolve's: the wall-clock completion
            // time is the headline ("" only on pre-persistence projects).
            if (!j.finished_at.empty())
                return QStringLiteral("Finished %1").arg(QString::fromStdString(j.finished_at));
            return QStringLiteral("Completed in %1").arg(format_duration(j.elapsed_seconds));
        case S::Failed: return QStringLiteral("Failed");
        case S::Cancelled: return QStringLiteral("Cancelled");
        case S::Rendering: {
            if (j.elapsed_seconds > 0.0 && j.progress > 0.05) {
                const double remaining = j.elapsed_seconds * (1.0 - j.progress) / j.progress;
                return QStringLiteral("Time remaining: ~%1").arg(format_duration(remaining));
            }
            return QStringLiteral("Rendering…");
        }
    }
    return QStringLiteral("Queued");
}

// Resolution summary for the job's primary line, e.g. "2160p", "1440p",
// "1080p", or the raw string ("Custom", "Timeline Resolution", ...).
QString res_summary(const canvas::core::DeliverSettings& s) {
    const QString r = QString::fromStdString(s.video.resolution);
    if (r.startsWith("3840 x 2160")) return QStringLiteral("2160p");
    if (r.startsWith("2560 x 1440")) return QStringLiteral("1440p");
    if (r.startsWith("1920 x 1080")) return QStringLiteral("1080p");
    if (r.startsWith("1280 x 720")) return QStringLiteral("720p");
    return r;
}

// Grip-dots drag affordance (3x2 dot grid), row-reorder cue.
QPixmap grip_pixmap() {
    QPixmap pm(14, 20);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(grip_pixmap_color()));
    for (int row = 0; row < 3; ++row)
        for (int col = 0; col < 2; ++col)
            p.drawEllipse(QPoint(3 + col * 8, 4 + row * 6), 2, 2);
    return pm;
}

}  // namespace

RenderQueuePanel::RenderQueuePanel(QWidget* parent) : QWidget(parent) {
    build();
    refresh();
}

void RenderQueuePanel::build() {
    setAutoFillBackground(false);
    apply_theme_style(this, [] {
        const ThemeTokens& t = tokens();
        return QStringLiteral("QWidget{background-color:%1;}")
            .arg(css(t.surface));
    });
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(10, 10, 10, 10);
    root->setSpacing(8);

    auto* title = new QLabel(tr("Render Queue"));
    apply_theme_style(title, [] {
        return QStringLiteral("color:%1;font-size:13px;font-weight:600;")
            .arg(css(tokens().ink));
    });
    root->addWidget(title);

    summary_ = new QLabel(tr("No jobs in queue."));
    apply_theme_style(summary_, [] {
        return QStringLiteral("color:%1;font-size:11px;").arg(css(tokens().ink_muted));
    });
    root->addWidget(summary_);

    list_ = new QListWidget(this);
    apply_theme_style(list_, &rq_list_style);
    list_->setSelectionMode(QAbstractItemView::SingleSelection);
    list_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    root->addWidget(list_, 1);

    // Thin pill progress bar at the bottom of the box, just above the buttons.
    auto* progress_row = new QHBoxLayout;
    progress_row->setSpacing(8);
    overall_ = new QProgressBar(this);
    overall_->setRange(0, 1000);
    overall_->setValue(0);
    overall_->setTextVisible(false);
    overall_->setFixedHeight(6);
    apply_theme_style(overall_, &rq_progress_style);
    progress_row->addWidget(overall_, 1);
    overall_pct_ = new QLabel(QStringLiteral("0%"), this);
    apply_theme_style(overall_pct_, [] {
        return QStringLiteral("color:%1;font-size:11px;").arg(css(tokens().ink_muted));
    });
    overall_pct_->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    progress_row->addWidget(overall_pct_);
    root->addLayout(progress_row);

    auto* buttons = new QHBoxLayout;
    buttons->setSpacing(6);
    render_all_ = new QPushButton(tr("Render"));
    apply_theme_style(render_all_, &rq_render_btn_style);
    auto* cancel_all = new QPushButton(tr("Cancel All"));
    apply_theme_style(cancel_all, &rq_cancel_btn_style);
    auto* clear = new QPushButton(tr("Clear Queue"));
    clear->setToolTip(tr("Remove every job that isn't currently rendering."));
    apply_theme_style(clear, &rq_clear_btn_style);
    // Keep the buttons at their natural size — without this they absorb all the
    // slack when the dock is resized and stretch absurdly wide.
    for (QPushButton* b : {render_all_, cancel_all, clear})
        b->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    buttons->addWidget(render_all_);
    buttons->addWidget(cancel_all);
    buttons->addWidget(clear);
    buttons->addStretch(1);
    root->addLayout(buttons);

    connect(render_all_, &QPushButton::clicked, this, &RenderQueuePanel::render_all_clicked);
    connect(cancel_all, &QPushButton::clicked, this, &RenderQueuePanel::cancel_all_clicked);
    connect(clear, &QPushButton::clicked, this, &RenderQueuePanel::clear_queued_clicked);
}

namespace {

QToolButton* make_row_action(const QString& icon_name, const QString& tip) {
    auto* b = new QToolButton;
    b->setIcon(canvas::gui::icon(icon_name.toLatin1().constData()));
    b->setIconSize(QSize(13, 13));
    b->setToolTip(tip);
    b->setCursor(Qt::PointingHandCursor);
    b->setFixedSize(20, 20);
    b->setAutoRaise(true);
    apply_theme_style(b, &rq_row_action_style);
    return b;
}

void create_row(RenderQueuePanel::JobRow& r, const canvas::core::RenderJob& j,
                QListWidget* list, RenderQueuePanel* panel) {
    auto* widget = new QWidget(list);
    auto* v = new QVBoxLayout(widget);
    v->setContentsMargins(6, 6, 6, 6);
    v->setSpacing(3);

    // Header row: drag grip · job label · status · edit/close actions.
    auto* header = new QHBoxLayout;
    header->setSpacing(8);
    auto* grip = new QLabel(widget);
    grip->setPixmap(grip_pixmap());
    header->addWidget(grip);
    auto* job_label = new QLabel(RenderQueuePanel::tr("Job %1").arg(j.id), widget);
    job_label->setStyleSheet(QStringLiteral("color:%1;font-size:12px;font-weight:700;")
                                     .arg(css(tokens().ink)));
    header->addWidget(job_label);
    header->addStretch(1);
    r.status = new QLabel(widget);
    r.status->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    r.status->setSizePolicy(QSizePolicy::Minimum, QSizePolicy::Preferred);
    header->addWidget(r.status);
    QToolButton* edit_btn = make_row_action("edit", RenderQueuePanel::tr("Edit settings"));
    QToolButton* close_btn = make_row_action("close", RenderQueuePanel::tr("Remove job"));
    header->addWidget(edit_btn);
    header->addWidget(close_btn);
    v->addLayout(header);

    // Content row: film-strip thumbnail · settings summary / output path.
    auto* content = new QHBoxLayout;
    content->setSpacing(8);
    auto* thumb = new QLabel(widget);
    thumb->setPixmap(canvas::gui::raw_icon("film-strip").pixmap(QSize(30, 24)));
    content->addWidget(thumb);
    auto* txt = new QVBoxLayout;
    txt->setSpacing(1);
    r.primary = new QLabel(widget);
    r.primary->setStyleSheet(QStringLiteral("color:%1;font-size:12px;font-weight:600;")
                                        .arg(css(tokens().ink)));
    txt->addWidget(r.primary);
    r.path = new QLabel(widget);
    r.path->setStyleSheet(QStringLiteral("color:%1;font-size:10px;")
                                    .arg(css(tokens().ink_muted)));
    r.path->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    txt->addWidget(r.path);
    content->addLayout(txt, 1);
    v->addLayout(content);

    const uint64_t id = j.id;
    QObject::connect(close_btn, &QToolButton::clicked, panel,
            [panel, id] { emit panel->job_remove_clicked(id); });
    QObject::connect(edit_btn, &QToolButton::clicked, panel,
            [panel, id] { emit panel->job_edit_clicked(id); });

    r.item = new QListWidgetItem(list);
    r.item->setSizeHint(widget->sizeHint());
    list->setItemWidget(r.item, widget);
}

}  // namespace

void RenderQueuePanel::set_queue(canvas::core::RenderQueue* queue) {
    queue_ = queue;
    refresh();
}

void RenderQueuePanel::refresh() {
    if (!queue_) return;
    const auto jobs = queue_->jobs();

    int running = 0, done = 0, queued = 0, dead = 0;
    double total_progress = 0.0;

    QHash<uint64_t, JobRow> next_rows;
    for (const auto& j : jobs) {
        JobRow r;
        auto cached = rows_.find(j.id);
        if (cached != rows_.end()) {
            r.item = cached->item;
            r.status = cached->status;
            r.primary = cached->primary;
            r.path = cached->path;
            rows_.erase(cached);
        } else {
            create_row(r, j, list_, this);
        }

        r.status->setText(status_text(j));
        switch (j.status) {
            case canvas::core::RenderJob::Status::Rendering:
                r.status->setStyleSheet(QStringLiteral("color:%1;font-size:11px;")
                                            .arg(css(tokens().accent_text)));
                break;
            case canvas::core::RenderJob::Status::Failed:
                r.status->setStyleSheet(QStringLiteral("color:%1;font-size:11px;")
                                            .arg(css(tokens().danger)));
                break;
            default:
                r.status->setStyleSheet(QStringLiteral("color:%1;font-size:11px;")
                                            .arg(css(tokens().ink_muted)));
                break;
        }
        r.path->setToolTip(QString::fromStdString(j.output_path));
        r.path->setText(QString::fromStdString(j.output_path));
        r.primary->setText(res_summary(j.settings) + QStringLiteral("  |  Timeline 1"));
        if (auto* w = list_->itemWidget(r.item))
            r.item->setSizeHint(w->sizeHint());
        next_rows.insert(j.id, r);

        using S = canvas::core::RenderJob::Status;
        if (j.status == S::Queued) ++queued;
        else if (j.status == S::Rendering) ++running;
        else if (j.status == S::Failed || j.status == S::Cancelled) { ++dead; continue; }
        else ++done;
        if (j.status == S::Rendering) total_progress += j.progress;
        else if (j.status == S::Completed) total_progress += 1.0;
    }

    // Rows whose jobs vanished (cleared/cancelled) get dropped.
    for (auto it = rows_.begin(); it != rows_.end();) {
        delete it->item;
        it = rows_.erase(it);
    }
    rows_ = std::move(next_rows);

    const int total = (int)jobs.size();
    const int live = total - dead;
    const double pct = live > 0 ? total_progress / live : 0.0;
    if (total > 0) {
        summary_->setText(tr("%1 jobs — %2 queued, %3 running, %4 done")
                              .arg(total).arg(queued).arg(running).arg(done));
        overall_->setValue((int)std::lround(pct * 1000.0));
        overall_pct_->setText(tr("%1%").arg((int)std::lround(pct * 100.0)));
        // "Render" is the manual start trigger: enable it whenever there is
        // staged (Queued) work and nothing is actively Rendering.
        render_all_->setEnabled(running == 0 && queued > 0);
    } else {
        summary_->setText(tr("No jobs in queue."));
        overall_->setValue(0);
        overall_pct_->setText(QStringLiteral("0%"));
        render_all_->setEnabled(false);
    }
}

}  // namespace canvas::gui