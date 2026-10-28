#pragma once
#include <QImage>
#include <QString>

namespace community {
QImage inviteQr(const QString& link);
QString readInviteQr(const QImage& image);
} // namespace community
