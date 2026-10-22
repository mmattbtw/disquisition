#include "community/model.h"

#include <openssl/crypto.h>

#include <QJsonDocument>
#include <QMessageAuthenticationCode>
#include <QUuid>
#include <algorithm>
#include <stdexcept>

namespace community {
namespace {
constexpr auto kOperationDomain = "disquisition/community-operation/v1\n";
constexpr auto kIntentDomain = "disquisition/community-join/v1\n";
void require(bool condition, const char* reason) {
    if (!condition)
        throw std::runtime_error(reason);
}
bool uuid(const QString& text) {
    return !QUuid(text).isNull() && QUuid(text).toString(QUuid::WithoutBraces) == text;
}
bool label(const QString& text, int maximum = 80) {
    return !text.trimmed().isEmpty() && text.size() <= maximum &&
           std::none_of(text.begin(), text.end(),
                        [](QChar ch) { return ch.category() == QChar::Other_Control; });
}
QString operationId(QJsonObject object) {
    object.remove("signature");
    return digest(canonical(object));
}
bool validIntent(const QJsonObject& intent, const QString& id) {
    return intent.value("community").toString() == id &&
           isPeerId(intent.value("peer").toString()) &&
           label(intent.value("name").toString(), 64) &&
           validEndpoint(intent.value("endpoint").toString()) &&
           isPeerId(intent.value("invite").toString()) &&
           verifyObject(intent, intent.value("peer").toString(), kIntentDomain);
}
bool inviteProof(QJsonObject intent, const QJsonObject& grant) {
    const auto proof = QByteArray::fromHex(intent.take("proof").toString().toLatin1());
    intent.remove("signature");
    const auto code = grant.value("data").toObject().value("code").toString();
    const auto expected = QMessageAuthenticationCode::hash(canonical(intent), code.toUtf8(),
                                                           QCryptographicHash::Sha256);
    return proof.size() == expected.size() &&
           CRYPTO_memcmp(proof.constData(), expected.constData(), proof.size()) == 0;
}
} // namespace

QString freshId() {
    return QUuid::createUuid().toString(QUuid::WithoutBraces);
}

bool validEndpoint(const QString& endpoint) {
    const QUrl url(endpoint, QUrl::StrictMode);
    return endpoint.size() <= 512 && url.isValid() && !url.host().isEmpty() &&
           (url.scheme() == "tcp" || url.scheme() == "relay") && url.port() > 0 &&
           url.port() <= 65535 && url.userInfo().isEmpty() && url.query().isEmpty() &&
           url.fragment().isEmpty() && (url.path().isEmpty() || url.path() == "/");
}

QString Invite::toLink() const {
    const QJsonObject object{
        {"v", 1}, {"community", community}, {"owner", owner}, {"code", code}, {"seeds", seeds}};
    return "disquisition://join/" +
           QString::fromLatin1(canonical(object).toBase64(QByteArray::Base64UrlEncoding |
                                                          QByteArray::OmitTrailingEquals));
}

Invite Invite::fromLink(const QString& link) {
    const QString prefix = "disquisition://join/";
    require(link.startsWith(prefix) && link.size() <= 8192, "invalid community invite link");
    const auto decoded = QByteArray::fromBase64Encoding(
        link.mid(prefix.size()).toLatin1(),
        QByteArray::Base64UrlEncoding | QByteArray::AbortOnBase64DecodingErrors);
    require(bool(decoded), "invalid invite encoding");
    QJsonParseError error;
    const auto document = QJsonDocument::fromJson(decoded.decoded, &error);
    require(error.error == QJsonParseError::NoError && document.isObject(), "invalid invite data");
    const auto object = document.object();
    Invite result{object.value("community").toString(), object.value("owner").toString(),
                  object.value("code").toString(), object.value("seeds").toArray()};
    require(object.value("v").toInt() == 1 && uuid(result.community) && isPeerId(result.owner) &&
                result.code.size() == 32 && !result.seeds.isEmpty() && result.seeds.size() <= 8,
            "unsupported or incomplete community invite");
    for (const auto& seed : result.seeds) {
        const auto route = seed.toObject();
        require(isPeerId(route.value("peer").toString()) &&
                    validEndpoint(route.value("endpoint").toString()),
                "invalid invite endpoint");
    }
    return result;
}

Model Model::create(const Identity& identity, const QString& communityName,
                    const QString& memberName, const QString& endpoint) {
    require(label(communityName) && label(memberName, 64) && validEndpoint(endpoint),
            "a community needs a name and a reachable endpoint");
    Model result;
    result.id = freshId();
    result.owner = identity.id();
    QJsonArray channels;
    channels.append(
        QJsonObject{{"id", freshId()}, {"name", "general"}, {"type", "text"}, {"topic", ""}});
    channels.append(
        QJsonObject{{"id", freshId()}, {"name", "Lounge"}, {"type", "voice"}, {"topic", ""}});
    const QJsonObject data{{"name", communityName},
                           {"memberName", memberName},
                           {"endpoint", endpoint},
                           {"channels", channels}};
    auto operation = signedObject({{"v", 1},
                                   {"community", result.id},
                                   {"author", result.owner},
                                   {"clock", 0},
                                   {"parents", QJsonArray{}},
                                   {"kind", "create"},
                                   {"data", data}},
                                  identity, kOperationDomain);
    result.operations_.insert(operationId(operation), operation);
    result.rebuild();
    return result;
}

QJsonObject Model::toJson() const {
    QJsonArray operations;
    for (const auto& operation : operations_)
        operations.append(operation);
    return {{"v", 1}, {"id", id}, {"owner", owner}, {"operations", operations}};
}

Model Model::fromJson(const QJsonObject& object, const QString& expectedId,
                      const QString& expectedOwner) {
    Model model;
    model.id = object.value("id").toString();
    model.owner = object.value("owner").toString();
    require(uuid(model.id) && isPeerId(model.owner) && object.value("v").toInt() == 1 &&
                (expectedId.isEmpty() || model.id == expectedId) &&
                (expectedOwner.isEmpty() || model.owner == expectedOwner),
            "community identity mismatch");
    const auto operations = object.value("operations").toArray();
    require(!operations.isEmpty() && operations.size() <= kMaxOperations &&
                canonical(object).size() <= kMaxMetadataBytes,
            "community metadata exceeds its limit");
    for (const auto& value : operations) {
        require(value.isObject(), "invalid metadata operation");
        const auto operation = value.toObject();
        require(operation.value("v").toInt() == 1 &&
                    operation.value("community").toString() == model.id &&
                    isPeerId(operation.value("author").toString()) &&
                    operation.value("parents").isArray() && operation.value("data").isObject() &&
                    operation.value("clock").isDouble() &&
                    operation.value("clock").toDouble() >= 0 &&
                    operation.value("clock").toDouble() <= kMaxOperations &&
                    operation.value("clock").toDouble() == operation.value("clock").toInt() &&
                    verifyObject(operation, operation.value("author").toString(), kOperationDomain),
                "invalid metadata signature or operation");
        model.operations_.insert(operationId(operation), operation);
    }
    require(model.operations_.size() == operations.size(), "duplicate metadata operations");
    model.rebuild();
    return model;
}

bool Model::merge(const QJsonObject& snapshot) {
    const Model incoming = fromJson(snapshot, id, owner);
    Model merged = *this;
    for (auto it = incoming.operations_.cbegin(); it != incoming.operations_.cend(); ++it)
        merged.operations_.insert(it.key(), it.value());
    if (merged.operations_.size() == operations_.size())
        return false;
    require(merged.operations_.size() <= kMaxOperations &&
                canonical(merged.toJson()).size() <= kMaxMetadataBytes,
            "community metadata exceeds its limit");
    merged.rebuild();
    *this = std::move(merged);
    return true;
}

bool Model::ancestor(const QString& older, const QString& newer) const {
    if (older == newer)
        return true;
    QSet<QString> visited;
    QStringList pending{newer};
    while (!pending.isEmpty()) {
        const auto current = pending.takeLast();
        if (visited.contains(current))
            continue;
        visited.insert(current);
        for (const auto& parent : operations_.value(current).value("parents").toArray()) {
            if (parent.toString() == older)
                return true;
            pending.append(parent.toString());
        }
    }
    return false;
}

void Model::rebuild() {
    members.clear();
    channels.clear();
    priority.clear();
    revokedInvites.clear();
    banned.clear();
    invites_.clear();
    adminGrants_.clear();
    heads_.clear();
    icon.clear();
    name.clear();
    clock_ = 0;
    QStringList ordered = operations_.keys();
    std::sort(ordered.begin(), ordered.end(), [this](const auto& a, const auto& b) {
        const int ca = operations_[a].value("clock").toInt();
        const int cb = operations_[b].value("clock").toInt();
        return ca == cb ? a < b : ca < cb;
    });
    int roots = 0;
    QSet<QString> nonHeads;
    QList<QString> ownerRoles;
    for (const auto& key : ordered) {
        const auto operation = operations_.value(key);
        const auto parents = operation.value("parents").toArray();
        const int clock = operation.value("clock").toInt();
        if (operation.value("kind") == "create") {
            require(parents.isEmpty() && clock == 0 &&
                        operation.value("author").toString() == owner,
                    "invalid community origin");
            ++roots;
        }
        else {
            require(!parents.isEmpty(), "metadata operation has no ancestry");
            int parentClock = -1;
            QSet<QString> unique;
            for (const auto& value : parents) {
                const auto parent = value.toString();
                require(operations_.contains(parent) && !unique.contains(parent) &&
                            operations_[parent].value("clock").toInt() < clock,
                        "invalid metadata ancestry");
                unique.insert(parent);
                parentClock = std::max(parentClock, operations_[parent].value("clock").toInt());
                nonHeads.insert(parent);
            }
            require(clock == parentClock + 1, "invalid metadata clock");
        }
        clock_ = std::max(clock_, clock);
        heads_.insert(key);
        if (operation.value("kind") == "admin" && operation.value("author").toString() == owner)
            ownerRoles.append(key);
    }
    require(roots == 1, "community must have exactly one origin");
    heads_.subtract(nonHeads);
    // A revoked grant cannot authorize concurrent or later edits. Edits that
    // causally preceded its revocation remain part of the accepted history.
    const auto authorized = [&](const QString& key) {
        const auto operation = operations_.value(key);
        const auto author = operation.value("author").toString();
        if (author == owner)
            return true;
        const auto grantId = operation.value("authority").toString();
        if (!operations_.contains(grantId))
            return false;
        const auto grant = operations_.value(grantId);
        const auto data = grant.value("data").toObject();
        if (grant.value("author").toString() != owner || grant.value("kind") != "admin" ||
            data.value("peer").toString() != author || !data.value("enabled").toBool() ||
            !ancestor(grantId, key))
            return false;
        for (const auto& roleId : ownerRoles) {
            const auto role = operations_.value(roleId).value("data").toObject();
            if (role.value("peer").toString() == author && roleId != grantId &&
                !ancestor(roleId, grantId) && !ancestor(key, roleId))
                return false;
        }
        return true;
    };
    QMap<QString, QStringList> revocations;
    QMap<QString, QStringList> removals;
    for (const auto& key : ordered) {
        const auto operation = operations_.value(key);
        const auto data = operation.value("data").toObject();
        if (!authorized(key))
            continue;
        if (operation.value("kind") == "revoke" && isPeerId(data.value("hash").toString())) {
            revokedInvites.insert(data.value("hash").toString());
            revocations[data.value("hash").toString()].append(key);
        }
        if (operation.value("kind") == "kick" && isPeerId(data.value("peer").toString()) &&
            data.value("peer").toString() != owner) {
            banned.insert(data.value("peer").toString());
            removals[data.value("peer").toString()].append(key);
        }
    }
    const auto couldAct = [&](const QString& peer, const QString& operation) {
        for (const auto& removal : removals.value(peer))
            if (!ancestor(operation, removal))
                return false;
        return true;
    };
    for (const auto& key : ordered) {
        const auto operation = operations_.value(key);
        const auto kind = operation.value("kind").toString();
        const auto author = operation.value("author").toString();
        const auto data = operation.value("data").toObject();
        if (kind == "create") {
            require(label(data.value("name").toString()) &&
                        label(data.value("memberName").toString(), 64) &&
                        validEndpoint(data.value("endpoint").toString()),
                    "invalid community origin data");
            name = data.value("name").toString();
            members.insert(owner, {owner, data.value("memberName").toString(),
                                   data.value("endpoint").toString(), true});
            require(data.value("channels").toArray().size() <= 128, "too many channels");
            for (const auto& item : data.value("channels").toArray()) {
                const auto channel = item.toObject();
                const auto channelId = channel.value("id").toString();
                const auto type = channel.value("type").toString();
                require(uuid(channelId) && label(channel.value("name").toString()) &&
                            (type == "text" || type == "voice"),
                        "invalid initial channel");
                channels.insert(channelId, {channelId, channel.value("name").toString(), type, {}});
            }
            continue;
        }
        if (kind == "admit") {
            const auto intent = data.value("intent").toObject();
            const auto peer = intent.value("peer").toString();
            if (!members.contains(author) || !couldAct(author, key) || !couldAct(peer, key) ||
                !validIntent(intent, id) || members.contains(peer) || members.size() >= kMaxMembers)
                continue;
            const auto hash = intent.value("invite").toString();
            if (!invites_.contains(hash) || !inviteProof(intent, invites_.value(hash)) ||
                (!intent.value("host").toString().isEmpty() &&
                 intent.value("host").toString() != author))
                continue;
            bool revoked = false;
            for (const auto& revocation : revocations.value(hash))
                if (!ancestor(key, revocation))
                    revoked = true;
            if (revoked)
                continue;
            members.insert(peer, {peer, intent.value("name").toString(),
                                  intent.value("endpoint").toString(), false});
            continue;
        }
        if (kind == "profile") {
            if (members.contains(author) && couldAct(author, key) &&
                label(data.value("name").toString(), 64) &&
                validEndpoint(data.value("endpoint").toString())) {
                members[author].name = data.value("name").toString();
                members[author].endpoint = data.value("endpoint").toString();
            }
            continue;
        }
        if (!members.contains(author) || !couldAct(author, key) || !authorized(key))
            continue;
        if (kind == "admin" && author == owner) {
            const auto peer = data.value("peer").toString();
            if (peer != owner && members.contains(peer) && data.value("enabled").isBool()) {
                members[peer].admin = data.value("enabled").toBool();
                if (members[peer].admin)
                    adminGrants_[peer] = key;
                else
                    adminGrants_.remove(peer);
            }
        }
        else if (kind == "rename" && label(data.value("name").toString())) {
            name = data.value("name").toString();
        }
        else if (kind == "icon") {
            const auto bytes = QByteArray::fromBase64(data.value("bytes").toString().toLatin1());
            if (bytes.size() <= kMaxIconBytes)
                icon = bytes;
        }
        else if (kind == "channel") {
            const auto channelId = data.value("id").toString();
            const auto type = data.value("type").toString();
            if (uuid(channelId) && label(data.value("name").toString()) &&
                (type == "text" || type == "voice") &&
                data.value("topic").toString().size() <= 512 &&
                (channels.contains(channelId) || channels.size() < 128))
                channels[channelId] = {channelId, data.value("name").toString(), type,
                                       data.value("topic").toString()};
        }
        else if (kind == "delete-channel") {
            channels.remove(data.value("id").toString());
        }
        else if (kind == "invite" && isPeerId(data.value("hash").toString()) &&
                 data.value("code").toString().size() == 32 &&
                 digest(data.value("code").toString().toUtf8()) == data.value("hash").toString()) {
            invites_.insert(data.value("hash").toString(), operation);
        }
        else if (kind == "priority" && data.value("peers").isArray()) {
            QStringList next;
            for (const auto& value : data.value("peers").toArray())
                if (members.contains(value.toString()) && !next.contains(value.toString()))
                    next.append(value.toString());
            priority = next;
        }
    }
    for (const auto& peer : banned) {
        members.remove(peer);
        adminGrants_.remove(peer);
    }
}

bool Model::canAdmin(const QString& peer) const {
    return members.contains(peer) && (peer == owner || members.value(peer).admin);
}

QJsonObject Model::append(const Identity& author, const QString& kind, QJsonObject data) {
    QJsonArray parents;
    auto sorted = heads_.values();
    sorted.sort();
    for (const auto& head : sorted)
        parents.append(head);
    QJsonObject operation{{"v", 1},
                          {"community", id},
                          {"author", author.id()},
                          {"clock", clock_ + 1},
                          {"parents", parents},
                          {"kind", kind},
                          {"data", data}};
    if (author.id() != owner && adminGrants_.contains(author.id()))
        operation["authority"] = adminGrants_.value(author.id());
    operation = signedObject(operation, author, kOperationDomain);
    Model next = *this;
    next.operations_.insert(operationId(operation), operation);
    require(next.operations_.size() <= kMaxOperations &&
                canonical(next.toJson()).size() <= kMaxMetadataBytes,
            "community metadata exceeds its limit");
    next.rebuild();
    *this = std::move(next);
    return operation;
}

QJsonObject Model::change(const Identity& author, const QString& kind, QJsonObject data) {
    const QSet<QString> administrative{"rename", "icon",     "channel", "delete-channel", "invite",
                                       "revoke", "priority", "kick",    "admin"};
    require((kind == "profile" && isMember(author.id())) ||
                (administrative.contains(kind) && canAdmin(author.id())),
            "only the owner or an admin can make this change");
    require(kind != "admin" || author.id() == owner, "only the owner can assign admins");
    if (kind == "rename")
        require(label(data.value("name").toString()), "invalid community name");
    if (kind == "profile")
        require(label(data.value("name").toString(), 64) &&
                    validEndpoint(data.value("endpoint").toString()),
                "invalid member profile");
    if (kind == "channel")
        require(uuid(data.value("id").toString()) && label(data.value("name").toString()) &&
                    (data.value("type") == "text" || data.value("type") == "voice") &&
                    data.value("topic").toString().size() <= 512,
                "invalid channel");
    if (kind == "icon")
        require(QByteArray::fromBase64(data.value("bytes").toString().toLatin1()).size() <=
                    kMaxIconBytes,
                "community icon is too large");
    if (kind == "kick")
        require(data.value("peer").toString() != owner &&
                    members.contains(data.value("peer").toString()),
                "cannot remove the owner or an unknown member");
    if (kind == "admin")
        require(members.contains(data.value("peer").toString()) &&
                    data.value("peer").toString() != owner && data.value("enabled").isBool(),
                "invalid admin assignment");
    return append(author, kind, std::move(data));
}

QString Model::issueInvite(const Identity& author) {
    const auto code = randomCode();
    change(author, "invite", {{"hash", digest(code.toUtf8())}, {"code", code}});
    return code;
}

bool Model::validCode(const QString& code) const {
    const auto hash = digest(code.toUtf8());
    return code.size() == 32 && invites_.contains(hash) && !revokedInvites.contains(hash);
}

QJsonObject joinIntent(const Identity& peer, const Invite& invite, const QString& name,
                       const QString& endpoint, const QString& host, const QString& nonce) {
    QJsonObject intent{{"community", invite.community},
                       {"peer", peer.id()},
                       {"invite", digest(invite.code.toUtf8())},
                       {"name", name},
                       {"endpoint", endpoint},
                       {"host", host},
                       {"nonce", nonce.isEmpty() ? randomCode() : nonce}};
    intent["proof"] = QString::fromLatin1(
        QMessageAuthenticationCode::hash(canonical(intent), invite.code.toUtf8(),
                                         QCryptographicHash::Sha256)
            .toHex());
    return signedObject(intent, peer, kIntentDomain);
}

bool Model::acceptsIntent(const QString& host, const QJsonObject& intent) const {
    const auto peer = intent.value("peer").toString();
    const auto hash = intent.value("invite").toString();
    return isMember(host) && validIntent(intent, id) && invites_.contains(hash) &&
           !revokedInvites.contains(hash) && inviteProof(intent, invites_.value(hash)) &&
           (intent.value("host").toString().isEmpty() || intent.value("host").toString() == host) &&
           !banned.contains(peer) && (isMember(peer) || members.size() < kMaxMembers);
}

QJsonObject Model::admit(const Identity& hostIdentity, const QJsonObject& intent) {
    require(acceptsIntent(hostIdentity.id(), intent) && !isMember(intent.value("peer").toString()),
            "join rejected");
    return append(hostIdentity, "admit", {{"intent", intent}});
}

QString Model::revision() const {
    return digest(operations_.keys().join(',').toUtf8());
}

QString Model::host(const QSet<QString>& online) const {
    for (const auto& peer : priority)
        if (members.contains(peer) && online.contains(peer))
            return peer;
    if (online.contains(owner))
        return owner;
    for (auto it = members.cbegin(); it != members.cend(); ++it)
        if (online.contains(it.key()))
            return it.key();
    return {};
}
} // namespace community
