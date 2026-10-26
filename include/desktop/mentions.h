#pragma once

#include <QList>
#include <QString>
#include <QStringList>

struct Mention {
    qsizetype start = 0;
    qsizetype length = 0;
    QString member;

    bool everyone() const { return member.isEmpty(); }
};

// Case-insensitive, on word boundaries, so "me@matt.net" and "@mattress"
// mention nobody. The longest name wins; @everyone and @here win ties.
QList<Mention> findMentions(const QString& body, const QStringList& members);

enum class Mentioned { No, ViaEveryone, ByName };

Mentioned mentionOf(const QList<Mention>& mentions, const QString& member);

struct MentionQuery {
    qsizetype start = -1;
    QString prefix;
};

MentionQuery mentionQueryAt(const QString& text, qsizetype cursor);
