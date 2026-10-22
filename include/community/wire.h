#pragma once

#include <QByteArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QtEndian>
#include <stdexcept>

namespace community {
constexpr quint32 kMaxPacketBytes = 256 * 1024;
inline QByteArray frame(const QJsonObject& message) {
    const auto payload = QJsonDocument(message).toJson(QJsonDocument::Compact);
    if (payload.isEmpty() || payload.size() > kMaxPacketBytes)
        throw std::runtime_error("community frame is too large");
    QByteArray result(4, Qt::Uninitialized);
    qToBigEndian<quint32>(payload.size(), result.data());
    return result + payload;
}
inline bool takeFrame(QByteArray& bytes, QJsonObject& out) {
    if (bytes.size() < 4)
        return false;
    const auto size = qFromBigEndian<quint32>(bytes.constData());
    if (size == 0 || size > kMaxPacketBytes)
        throw std::runtime_error("invalid community frame size");
    if (bytes.size() < static_cast<qsizetype>(size) + 4)
        return false;
    QJsonParseError error;
    const auto document = QJsonDocument::fromJson(bytes.mid(4, size), &error);
    if (error.error != QJsonParseError::NoError || !document.isObject())
        throw std::runtime_error("invalid community frame");
    bytes.remove(0, size + 4);
    out = document.object();
    return true;
}
} // namespace community
