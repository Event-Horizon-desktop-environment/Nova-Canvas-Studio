#pragma once

#include <QColor>
#include <QIcon>

namespace canvas::gui {

QIcon icon(const char* name);

QIcon raw_icon(const char* name);

QIcon icon(const char* name, const QColor& normal);

}
