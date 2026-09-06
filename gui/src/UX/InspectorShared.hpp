#pragma once

// Shared inspector scaffold reused by every property category (splitplan
// refactor): the collapsible InspectorCategory group, one label|field|reset
// row, and the bounded double-spin builder. Extracted from ShellDocks.cpp so
// InspectorVisual.cpp builds its Transform/Composite categories with the exact
// same look without duplicating the helpers.

#include <QDoubleSpinBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QSize>
#include <QToolButton>
#include <QVBoxLayout>
#include <QWidget>

#include "UX/theme.hpp"

namespace canvas::gui {

// One collapsible property group, matching the standard inspector convention:
// enable dot | title | chevron | reset icon in the header, individual property
// rows in the body. When `has_enable` is true, a small circular enable toggle
// sits at the far left of the header (independent of the expand/collapse
// chevron), so sections like Speed Change / Equalizer can be switched on/off
// without opening or closing the group.
class InspectorCategory : public QWidget {
    Q_OBJECT

public:
    InspectorCategory(const QString& title, bool expanded, QWidget* parent = nullptr)
        : InspectorCategory(title, expanded, false, parent) {}

    InspectorCategory(const QString& title, bool expanded, bool has_enable, QWidget* parent = nullptr)
        : QWidget(parent) {
        auto* outer = new QVBoxLayout(this);
        outer->setContentsMargins(0, 0, 0, 0);
        outer->setSpacing(0);

        if (has_enable) {
            enable_ = new QToolButton(this);
            enable_->setCheckable(true);
            enable_->setChecked(true);
            enable_->setFixedSize(18, 18);
            enable_->setStyleSheet(
                QStringLiteral(
                    "QToolButton { border-radius: 9px; border: 1px solid #2A2F3C;"
                    "  background-color: #141A21; }"
                    "QToolButton:checked { background-color: #3B82F6; border-color: #3B82F6; }"));
            connect(enable_, &QToolButton::toggled, this,
                    [this](bool on) { emit feature_toggled(on); });
        }

        header_ = new QToolButton(this);
        header_->setStyleSheet(inspector_category_header_style());
        header_->setToolButtonStyle(Qt::ToolButtonTextOnly);
        header_->setCheckable(true);
        header_->setChecked(expanded);
        header_->setArrowType(expanded ? Qt::DownArrow : Qt::RightArrow);
        header_->setText(QStringLiteral("  \u25CF  ") + title);

        auto* reset = new QToolButton(this);
        reset->setIcon(icon("reset"));
        reset->setIconSize(QSize(14, 14));
        reset->setAutoRaise(true);
        reset->setToolTip(tr("Reset to default"));

        auto* header_row = new QWidget(this);
        auto* header_layout = new QHBoxLayout(header_row);
        header_layout->setContentsMargins(0, 0, 4, 0);
        header_layout->setSpacing(0);
        if (enable_) header_layout->addWidget(enable_);
        header_layout->addWidget(header_, 1);
        header_layout->addWidget(reset);
        header_row->setStyleSheet(QStringLiteral("background-color: #1A1D27; border-bottom: 1px solid #232833;"));

        body_ = new QWidget(this);
        body_->setStyleSheet(inspector_body_style());
        body_layout_ = new QVBoxLayout(body_);
        body_layout_->setContentsMargins(10, 8, 10, 10);
        body_layout_->setSpacing(8);
        body_->setVisible(expanded);

        outer->addWidget(header_row);
        outer->addWidget(body_);

        connect(header_, &QToolButton::toggled, this, [this](bool on) {
            header_->setArrowType(on ? Qt::DownArrow : Qt::RightArrow);
            body_->setVisible(on);
        });
    }

    QVBoxLayout* body_layout() { return body_layout_; }

    // Enable-toggle state; always true when the category has no enable toggle.
    [[nodiscard]] bool feature_enabled() const { return !enable_ || enable_->isChecked(); }
    void set_feature_enabled(bool on) {
        if (enable_) enable_->setChecked(on);
    }
    void set_feature_toggle_enabled(bool on) {
        if (enable_) enable_->setEnabled(on);
    }

signals:
    void feature_toggled(bool enabled);

private:
    QToolButton* enable_ = nullptr;
    QToolButton* header_ = nullptr;
    QWidget* body_ = nullptr;
    QVBoxLayout* body_layout_ = nullptr;
};

// One property row: label | field | optional per-property reset icon.
inline void add_property_row(QVBoxLayout* body, const QString& label, QWidget* field,
                             bool with_reset = true) {
    auto* row = new QHBoxLayout;
    row->setSpacing(6);
    auto* lbl = new QLabel(label);
    lbl->setMinimumWidth(78);
    lbl->setStyleSheet(QStringLiteral("color: #9AA0B0; font-size: 11px;"));
    row->addWidget(lbl);
    row->addWidget(field, 1);
    if (with_reset) {
        auto* reset = new QToolButton;
        reset->setIcon(icon("reset"));
        reset->setIconSize(QSize(14, 14));
        reset->setAutoRaise(true);
        reset->setFixedWidth(18);
        row->addWidget(reset);
    }
    body->addLayout(row);
}

// Bounded numeric property field, reference-style defaults.
inline QDoubleSpinBox* make_numeric(double lo, double hi, double val, QWidget* parent) {
    auto* s = new QDoubleSpinBox(parent);
    s->setRange(lo, hi);
    s->setValue(val);
    s->setDecimals(3);
    s->setMaximumWidth(90);
    return s;
}

}  // namespace canvas::gui