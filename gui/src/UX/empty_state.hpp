#pragma once

// Branded empty-state widget used by every placeholder surface (left-dock
// placeholder tabs, inspector placeholder pages). A quiet Nova-gold hero icon,
// a title at body/title scale, and an optional muted subtitle — re-tinted on
// theme switches so it can never drift from the active token set.

#include <QLabel>
#include <QPixmap>
#include <QVBoxLayout>
#include <QWidget>

#include "UX/theme.hpp"

namespace canvas::gui {

// Builds a vertically-centered empty state (icon, title, optional subtitle)
// inside `parent`. Returns the container widget; callers may add extra widgets
// to the returned layout handle is NOT exposed — use the page's own layout and
// embed the returned widget, or append actions via `layout()->addWidget`.
// `stretch_after` adds a trailing stretch below the copy (centered when set).
inline QWidget* build_empty_state(QWidget* parent, const char* icon_name,
                                  const QString& title, const QString& subtitle = QString(),
                                  bool stretch_after = true) {
    auto* host = new QWidget(parent);
    auto* layout = new QVBoxLayout(host);
    layout->setContentsMargins(24, 32, 24, 32);
    layout->setSpacing(8);

    auto* brand_icon = new QLabel(host);
    brand_icon->setAlignment(Qt::AlignCenter);
    const auto retint = [brand_icon, icon_name] {
        const ThemeTokens& t = tokens();
        brand_icon->setPixmap(icon(icon_name, with_alpha(t.accent, 170)).pixmap(48, 48));
    };
    retint();
    register_theme_reapply(retint);

    auto* title_label = new QLabel(title, host);
    title_label->setAlignment(Qt::AlignCenter);
    title_label->setWordWrap(true);
    apply_theme_style(title_label, [] {
        return QStringLiteral(
            "QLabel { color: %1; font-size: 16px; font-weight: 600; background: transparent; }")
            .arg(css(tokens().ink));
    });

    QLabel* subtitle_label = nullptr;
    if (!subtitle.isEmpty()) {
        subtitle_label = new QLabel(subtitle, host);
        subtitle_label->setAlignment(Qt::AlignCenter);
        subtitle_label->setWordWrap(true);
        apply_theme_style(subtitle_label, [] {
            return QStringLiteral(
                "QLabel { color: %1; font-size: 13px; font-weight: 400; background: transparent; }")
                .arg(css(tokens().ink_faint));
        });
    }

    layout->addStretch();
    layout->addWidget(brand_icon);
    layout->addSpacing(8);
    layout->addWidget(title_label);
    if (subtitle_label) {
        layout->addSpacing(4);
        layout->addWidget(subtitle_label);
    }
    if (stretch_after) layout->addStretch();
    return host;
}

}  // namespace canvas::gui