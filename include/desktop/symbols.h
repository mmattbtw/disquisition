#pragma once

#include <QColor>
#include <QIcon>
#include <QPixmap>
#include <QString>

// Filled Material Symbols bundled from resources/ui/icons, painted in one color.
namespace symbols {
// `name` is the symbol's file name without `.svg`, such as "mic_off".
QPixmap pixmap(const QString& name, const QColor& color, int size, qreal scale);
// Renders at whatever size and screen scale the widget asks for. A checkable
// button shows `checkedName` in `checkedColor` while it is checked.
QIcon icon(const QString& name, const QColor& color, const QString& checkedName = {},
           const QColor& checkedColor = {});
} // namespace symbols
