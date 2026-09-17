#pragma once

#include <QByteArray>
#include <QIcon>
#include <QListWidget>
#include <QWidget>

#include <vector>

class QTabWidget;

namespace canvas::gui {

enum class ToolboxKind { Title, Transition, Effect };

class ToolboxList : public QListWidget {
    Q_OBJECT
public:
    explicit ToolboxList(QWidget* parent = nullptr);

    QListWidgetItem* add_item(const QString& text, ToolboxKind kind, const QString& meta = {},
                              const char* mime = nullptr,
                              const QByteArray& payload = QByteArray(),
                              const char* icon_name = nullptr);

protected:
    void startDrag(Qt::DropActions supported) override;
};

class ToolboxWidget : public QWidget {
    Q_OBJECT
public:
    explicit ToolboxWidget(QWidget* parent = nullptr);

    struct TitlePreset {
        const char* id = nullptr;
        const char* label = nullptr;
        const char* sample = nullptr;
        float size = 0.0f;
    };
    static const std::vector<TitlePreset>& title_presets();

private:
    QWidget* build_effects_tab();
    QWidget* build_titles_tab();
    QWidget* build_transitions_tab();
    QWidget* build_more_tab();

    QTabWidget* tabs_ = nullptr;
    ToolboxList* effects_ = nullptr;
    ToolboxList* titles_ = nullptr;
    ToolboxList* transitions_ = nullptr;
};

}
