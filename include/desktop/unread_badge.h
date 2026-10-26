#pragma once

#include <QImage>
#include <QString>

struct UnreadBadge {
    int mentions = 0;
    bool unread = false;

    bool isEmpty() const { return mentions == 0 && !unread; }
    QString label() const;

    bool operator==(const UnreadBadge&) const = default;
};

QString badgedTitle(const QString& title, const UnreadBadge& badge);

// Needs a QGuiApplication for the font.
QImage drawBadge(const QImage& icon, const UnreadBadge& badge);
