#include "desktop/mentions.h"

#include <QStringView>
#include <algorithm>

namespace {

const QString kEveryone = QStringLiteral("everyone");
const QString kHere = QStringLiteral("here");

bool continuesWord(QChar character) {
    return character.isLetterOrNumber() || character == u'_' || character == u'-';
}

bool canStartMention(const QString& text, qsizetype at) {
    return at == 0 || !continuesWord(text.at(at - 1));
}

bool matchesAt(const QString& body, qsizetype at, const QString& name) {
    const qsizetype end = at + 1 + name.size();
    return !name.isEmpty() && end <= body.size() &&
           QStringView(body).sliced(at + 1, name.size()).compare(name, Qt::CaseInsensitive) == 0 &&
           (end == body.size() || !continuesWord(body.at(end)));
}

}  // namespace

QList<Mention> findMentions(const QString& body, const QStringList& members) {
    QList<Mention> mentions;
    qsizetype at = body.indexOf(u'@');
    while (at >= 0) {
        if (!canStartMention(body, at)) {
            at = body.indexOf(u'@', at + 1);
            continue;
        }
        QStringList matched;
        qsizetype longest = 0;
        for (const QString& member : members) {
            if (member.size() < longest || !matchesAt(body, at, member)) continue;
            if (member.size() > longest) {
                longest = member.size();
                matched.clear();
            }
            matched.append(member);
        }
        for (const QString& everyone : {kEveryone, kHere}) {
            if (everyone.size() >= longest && matchesAt(body, at, everyone)) {
                longest = everyone.size();
                matched = {QString()};
            }
        }
        if (matched.isEmpty()) {
            at = body.indexOf(u'@', at + 1);
            continue;
        }
        const QStringView typed = QStringView(body).sliced(at + 1, longest);
        const auto exact = std::find_if(matched.cbegin(), matched.cend(),
                                        [typed](const QString& name) { return typed == name; });
        if (exact != matched.cend()) matched = {*exact};
        for (const QString& member : std::as_const(matched)) {
            mentions.append({at, longest + 1, member});
        }
        at = body.indexOf(u'@', at + 1 + longest);
    }
    return mentions;
}

Mentioned mentionOf(const QList<Mention>& mentions, const QString& member) {
    Mentioned result = Mentioned::No;
    for (const Mention& mention : mentions) {
        if (mention.everyone()) {
            result = Mentioned::ViaEveryone;
        } else if (mention.member == member) {
            return Mentioned::ByName;
        }
    }
    return result;
}

MentionQuery mentionQueryAt(const QString& text, qsizetype cursor) {
    cursor = std::clamp<qsizetype>(cursor, 0, text.size());
    for (qsizetype at = cursor - 1; at >= 0; --at) {
        const QChar character = text.at(at);
        if (character.isSpace()) break;
        if (character == u'@' && canStartMention(text, at)) {
            return {at, text.mid(at + 1, cursor - at - 1)};
        }
    }
    return {};
}
