#include "UX/ActionSearch.hpp"

#include "UX/theme.hpp"

#include <QAction>
#include <QBoxLayout>
#include <QKeyEvent>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMenuBar>
#include <QVBoxLayout>

#include <functional>
#include <string>
#include <utility>

namespace canvas::gui {

namespace {

QString strip_mnemonic(QString text) {
    QString out;
    out.reserve(text.size());
    for (int i = 0; i < text.size(); ++i) {
        if (text[i] == QLatin1Char('&')) {
            if (i + 1 < text.size() && text[i + 1] == QLatin1Char('&')) {
                out += QLatin1Char('&');
                ++i;
            }
            continue;
        }
        out += text[i];
    }
    return out.trimmed();
}

QString search_style() {
    const ThemeTokens& t = tokens();
    return QStringLiteral(
               "QDialog{background:%1;}"
               "QLineEdit{background:%2;border:1px solid %3;border-radius:8px;"
               "padding:8px 10px;color:%4;font-size:14px;}"
               "QListWidget{background:%2;border:1px solid %3;border-radius:8px;"
               "color:%4;font-size:13px;}"
               "QListWidget::item{padding:7px 10px;border-radius:6px;}"
               "QListWidget::item:selected{background:%5;color:%4;}")
        .arg(css(t.surface), css(t.surface_low), css(t.border_soft), css(t.ink),
             css(t.accent_soft));
}

}

ActionSearch::ActionSearch(QMenuBar* menubar, QWidget* parent)
    : QDialog(parent), menubar_(menubar) {
    setWindowTitle(tr("Find Action"));
    resize(480, 340);
    apply_theme_style(this, &search_style);

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(14, 14, 14, 14);
    root->setSpacing(10);

    query_ = new QLineEdit(this);
    query_->setPlaceholderText(tr("Type a command name…  (Esc to close)"));
    root->addWidget(query_);

    results_ = new QListWidget(this);
    results_->setSelectionMode(QAbstractItemView::SingleSelection);
    root->addWidget(results_, 1);

    connect(query_, &QLineEdit::textChanged, this, &ActionSearch::apply_filter);
    connect(query_, &QLineEdit::returnPressed, this, &ActionSearch::run_current);
    connect(results_, &QListWidget::itemActivated, this, [this] { run_current(); });

    apply_filter({});
}

void ActionSearch::collect_actions() {
    registry_ = canvas::core::actions::Registry{};
    actions_.clear();
    if (!menubar_) return;

    std::function<void(QMenu*, const QString&)> walk = [&](QMenu* menu,
                                                           const QString& category) {
        for (QAction* a : menu->actions()) {
            if (a->isSeparator()) continue;
            if (QMenu* sub = a->menu()) {
                walk(sub, sub->title());
                continue;
            }
            if (a->text().isEmpty()) continue;

            const QString title = strip_mnemonic(a->text());
            if (title.isEmpty()) continue;

            canvas::core::actions::Action act;
            act.id = category.toStdString() + "/" + title.toStdString();
            act.title = title.toStdString();
            act.category = category.toStdString();
            const QString shortcut = a->shortcut().toString();
            if (!shortcut.isEmpty()) act.shortcut = shortcut.toStdString();
            registry_.add(std::move(act));
            actions_.insert(QString::fromStdString(act.id), a);
        }
    };

    const auto top = menubar_->actions();
    for (QAction* t : top) {
        if (QMenu* m = t->menu()) walk(m, strip_mnemonic(t->text()));
    }
}

void ActionSearch::apply_filter(const QString& query) {
    results_->clear();
    const auto hits = registry_.search(query.toStdString(), 14);
    for (const auto& hit : hits) {
        QAction* action = actions_.value(QString::fromStdString(hit.action->id));
        if (!action) continue;
        const QString title = QString::fromStdString(hit.action->title);
        const QString shortcut = QString::fromStdString(hit.action->shortcut);

        QString text = title;
        if (!shortcut.isEmpty())
            text += QStringLiteral("\t%1").arg(shortcut);
        auto* item = new QListWidgetItem(text, results_);
        item->setToolTip(QStringLiteral("%1  ·  %2")
                             .arg(title, QString::fromStdString(hit.action->category)));
        item->setData(Qt::UserRole, QString::fromStdString(hit.action->id));
    }
    if (results_->count() > 0) results_->setCurrentRow(0);
}

void ActionSearch::run_current() {
    QListWidgetItem* item = results_->currentItem();
    if (!item) {
        if (results_->count() > 0) item = results_->item(0);
    }
    if (!item) return;
    QAction* action = actions_.value(item->data(Qt::UserRole).toString());
    if (!action) return;
    accept();
    action->trigger();
}

}
