#pragma once

#include <QJsonArray>
#include <QMap>
#include <QSet>
#include <QUrl>

#include "community/identity.h"

namespace community {

constexpr qsizetype kMaxMetadataBytes = 8 * 1024 * 1024;
constexpr int kMaxOperations = 8192;
constexpr int kMaxIconBytes = 128 * 1024;
constexpr int kMaxMembers = 1024;

struct Member {
    QString id;
    QString name;
    QString endpoint;
    bool admin = false;
};
struct Channel {
    QString id;
    QString name;
    QString type; // text or voice
    QString topic;
};
struct Invite {
    QString community;
    QString owner;
    QString code;
    QJsonArray seeds; // {peer, endpoint}. These are routing hints, never authority.
    QString toLink() const;
    static Invite fromLink(const QString& link);
};

// Immutable, signed operations form a causally ordered log. Merging unions
// operations and derives the same state on every client, including after a split.
class Model {
public:
    static Model create(const Identity& owner, const QString& name, const QString& memberName,
                        const QString& endpoint);
    static Model fromJson(const QJsonObject& object, const QString& expectedId = {},
                          const QString& expectedOwner = {});
    QJsonObject toJson() const;
    bool merge(const QJsonObject& snapshot); // atomic; throws on malformed input
    QJsonObject change(const Identity& author, const QString& kind, QJsonObject data);
    QJsonObject admit(const Identity& host, const QJsonObject& intent);
    bool acceptsIntent(const QString& host, const QJsonObject& intent) const;
    QString issueInvite(const Identity& author);
    bool validCode(const QString& code) const;
    bool isMember(const QString& peer) const { return members.contains(peer); }
    bool canAdmin(const QString& peer) const;
    QString revision() const;
    QString host(const QSet<QString>& online) const;
    QStringList operationIds() const { return operations_.keys(); }
    QStringList inviteIds() const { return invites_.keys(); }

    QString id;
    QString owner;
    QString name;
    QByteArray icon;
    QMap<QString, Member> members;
    QMap<QString, Channel> channels;
    QStringList priority;
    QSet<QString> revokedInvites;
    QSet<QString> banned;

private:
    void rebuild();
    QJsonObject append(const Identity& author, const QString& kind, QJsonObject data);
    bool ancestor(const QString& older, const QString& newer) const;
    QMap<QString, QJsonObject> operations_;
    QMap<QString, QJsonObject> invites_;
    QMap<QString, QString> adminGrants_;
    QSet<QString> heads_;
    int clock_ = 0;
};

QJsonObject joinIntent(const Identity& peer, const Invite& invite, const QString& name,
                       const QString& endpoint, const QString& host = {},
                       const QString& nonce = {});
bool validEndpoint(const QString& endpoint);
// Relay settings accept a hostname, host:port, or the internal relay:// URI.
QString normalizeRelayEndpoint(const QString& address);
QString freshId();

} // namespace community
