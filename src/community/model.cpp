#include "community/model.h"

#include <openssl/crypto.h>

#include <QJsonDocument>
#include <QMessageAuthenticationCode>
#include <QUuid>
#include <algorithm>
#include <optional>
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
bool profileDetails(const QJsonObject& data) {
    const auto bio = data.value("bio");
    const auto bioText = bio.toString();
    if (!bio.isUndefined() && (!bio.isString() || bioText.size() > kMaxBioLength ||
                               std::any_of(bioText.begin(), bioText.end(), [](QChar ch) {
                                   return ch.category() == QChar::Other_Control && ch != '\n' &&
                                          ch != '\t';
                               })))
        return false;
    const auto avatar = data.value("avatar");
    if (avatar.isUndefined())
        return true;
    if (!avatar.isString() || avatar.toString().size() > ((kMaxAvatarBytes + 2) / 3) * 4)
        return false;
    const auto decoded = QByteArray::fromBase64Encoding(avatar.toString().toLatin1(),
                                                        QByteArray::AbortOnBase64DecodingErrors);
    return bool(decoded) && decoded.decoded.size() <= kMaxAvatarBytes;
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

QJsonObject Profile::toJson() const {
    return {{"name", name}, {"bio", bio}, {"avatar", QString::fromLatin1(avatar.toBase64())}};
}
Profile Profile::fromJson(const QJsonObject& object) {
    require(label(object.value("name").toString(), 64) && profileDetails(object),
            "A profile needs a display name of 1–64 characters, a bio of at most 500 characters, "
            "and a picture of at most 32 KB.");
    return {object.value("name").toString(), object.value("bio").toString(),
            QByteArray::fromBase64(object.value("avatar").toString().toLatin1())};
}

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

QString normalizeRelayEndpoint(const QString& address) {
    const auto input = address.trimmed();
    if (input.isEmpty())
        return {};
    QUrl url(input.contains("://") ? input : "relay://" + input, QUrl::StrictMode);
    require(url.isValid() && url.scheme() == "relay",
            "Enter a relay hostname with an optional port.");
    if (url.port() == -1)
        url.setPort(3333);
    const auto result = url.toString();
    require(validEndpoint(result), "Enter a relay hostname with an optional port.");
    return result;
}

namespace {
// Base58 keeps links alphanumeric, so chat markdown and double-click selection
// leave them intact.
constexpr char kBase58[] = "123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz";
constexpr int kInviteVersion = 2;
constexpr int kOwnerNotSeed = 0xff;
const QStringList kInviteSchemes{"relay", "tcp"};

QString base58(const QByteArray& bytes) {
    QByteArray digits; // little-endian
    for (const auto byte : bytes) {
        int carry = static_cast<unsigned char>(byte);
        for (auto& digit : digits) {
            carry += digit * 256;
            digit = char(carry % 58);
            carry /= 58;
        }
        for (; carry; carry /= 58)
            digits.append(char(carry % 58));
    }
    QString result;
    for (qsizetype index = 0; index < bytes.size() && bytes[index] == 0; ++index)
        result += '1';
    for (auto digit = digits.crbegin(); digit != digits.crend(); ++digit)
        result += QLatin1Char(kBase58[int(*digit)]);
    return result;
}

QByteArray fromBase58(const QString& text) {
    static const QByteArray alphabet(kBase58);
    QByteArray bytes; // little-endian
    for (const auto character : text) {
        int carry = alphabet.indexOf(character.toLatin1());
        require(carry >= 0, "invalid invite encoding");
        for (auto& byte : bytes) {
            carry += static_cast<unsigned char>(byte) * 58;
            byte = char(carry & 0xff);
            carry >>= 8;
        }
        for (; carry; carry >>= 8)
            bytes.append(char(carry & 0xff));
    }
    for (qsizetype index = 0; index < text.size() && text[index] == '1'; ++index)
        bytes.append('\0');
    std::reverse(bytes.begin(), bytes.end());
    return bytes;
}

struct Reader {
    QByteArray bytes;
    qsizetype at = 0;
    QByteArray take(qsizetype size) {
        require(size <= bytes.size() - at, "incomplete community invite");
        at += size;
        return bytes.mid(at - size, size);
    }
    int byte() { return static_cast<unsigned char>(take(1)[0]); }
};

QByteArray peerBytes(const QString& peer) {
    require(isPeerId(peer), "invalid invite peer");
    return QByteArray::fromHex(peer.toLatin1());
}

// v1 links held base64url JSON, which always begins with `{"` and so `eyJ`.
// They stay readable for links already shared or saved as pending joins.
std::optional<Invite> legacyInvite(const QString& body) {
    if (!body.startsWith("eyJ"))
        return {};
    const auto decoded = QByteArray::fromBase64Encoding(
        body.toLatin1(), QByteArray::Base64UrlEncoding | QByteArray::AbortOnBase64DecodingErrors);
    if (!decoded)
        return {};
    const auto document = QJsonDocument::fromJson(decoded.decoded);
    const auto object = document.object();
    if (!document.isObject() || object.value("v").toInt() != 1)
        return {};
    return Invite{object.value("community").toString(), object.value("owner").toString(),
                  object.value("code").toString(), object.value("seeds").toArray()};
}

// v2: version, community UUID, code, then each distinct endpoint once, so peers
// behind the same relay share it. Seeds keep their dial order and name an
// endpoint by index. The owner is usually a seed and is then also an index.
Invite compactInvite(const QString& body) {
    require(body.size() <= 4096, "invalid community invite link");
    Reader reader{fromBase58(body)};
    require(reader.byte() == kInviteVersion, "unsupported or incomplete community invite");
    Invite result;
    result.community = QUuid::fromRfc4122(reader.take(16)).toString(QUuid::WithoutBraces);
    result.code = QString::fromLatin1(
        reader.take(24).toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals));
    QStringList endpoints;
    for (int count = reader.byte(); endpoints.size() < count;) {
        const auto scheme = reader.byte();
        require(scheme < kInviteSchemes.size(), "invalid invite endpoint");
        const auto high = reader.byte();
        const auto port = high << 8 | reader.byte();
        QUrl url;
        url.setScheme(kInviteSchemes[scheme]);
        url.setHost(QString::fromUtf8(reader.take(reader.byte())), QUrl::StrictMode);
        url.setPort(port);
        endpoints.append(url.toString());
    }
    for (int count = reader.byte(); result.seeds.size() < count;) {
        const auto peer = QString::fromLatin1(reader.take(32).toHex());
        const auto endpoint = reader.byte();
        require(endpoint < endpoints.size(), "invalid invite endpoint");
        result.seeds.append(QJsonObject{{"peer", peer}, {"endpoint", endpoints[endpoint]}});
    }
    const auto owner = reader.byte();
    require(owner == kOwnerNotSeed || owner < result.seeds.size(), "invalid invite owner");
    result.owner = owner == kOwnerNotSeed ? QString::fromLatin1(reader.take(32).toHex())
                                          : result.seeds[owner].toObject().value("peer").toString();
    require(reader.at == reader.bytes.size(), "invalid invite data");
    return result;
}
} // namespace

QString Invite::toLink() const {
    const auto id = QUuid(community);
    const auto secret = QByteArray::fromBase64Encoding(
        code.toLatin1(), QByteArray::Base64UrlEncoding | QByteArray::AbortOnBase64DecodingErrors);
    require(!id.isNull() && secret && secret.decoded.size() == 24 && !seeds.isEmpty() &&
                seeds.size() <= 8,
            "incomplete community invite");
    QByteArray bytes;
    bytes.append(char(kInviteVersion));
    bytes.append(id.toRfc4122());
    bytes.append(secret.decoded);
    QStringList endpoints;
    QByteArray table, routes;
    int ownerIndex = kOwnerNotSeed;
    for (const auto& value : seeds) {
        const auto seed = value.toObject();
        const auto peer = seed.value("peer").toString(),
                   endpoint = seed.value("endpoint").toString();
        if (!endpoints.contains(endpoint)) {
            const QUrl url(endpoint, QUrl::StrictMode);
            const auto host = url.host(QUrl::FullyEncoded).toUtf8();
            require(validEndpoint(endpoint) && host.size() <= 255, "invalid invite endpoint");
            table.append(char(kInviteSchemes.indexOf(url.scheme())));
            table.append(char(url.port() >> 8));
            table.append(char(url.port() & 0xff));
            table.append(char(host.size()));
            table.append(host);
            endpoints.append(endpoint);
        }
        if (peer == owner && ownerIndex == kOwnerNotSeed)
            ownerIndex = int(routes.size() / 33);
        routes.append(peerBytes(peer));
        routes.append(char(endpoints.indexOf(endpoint)));
    }
    bytes.append(char(endpoints.size()));
    bytes.append(table);
    bytes.append(char(seeds.size()));
    bytes.append(routes);
    bytes.append(char(ownerIndex));
    if (ownerIndex == kOwnerNotSeed)
        bytes.append(peerBytes(owner));
    return "disquisition://join/" + base58(bytes);
}

Invite Invite::fromLink(const QString& link) {
    const QString prefix = "disquisition://join/";
    require(link.startsWith(prefix) && link.size() <= 8192, "invalid community invite link");
    const auto body = link.mid(prefix.size());
    auto legacy = legacyInvite(body);
    const auto result = legacy ? std::move(*legacy) : compactInvite(body);
    require(uuid(result.community) && isPeerId(result.owner) &&
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
    deleted = false;
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
    QMap<QString, QStringList> bans;
    QMap<QString, QStringList> unbans;
    QMap<QString, QStringList> departures;
    QMap<QString, QString> admissions;
    QSet<QString> acceptedIntents;
    for (const auto& key : ordered) {
        const auto operation = operations_.value(key);
        const auto data = operation.value("data").toObject();
        const auto author = operation.value("author").toString();
        if (operation.value("kind") == "leave" && author != owner)
            departures[author].append(key);
        if (operation.value("kind") == "delete" && author == owner)
            deleted = true;
        if (!authorized(key))
            continue;
        if (operation.value("kind") == "revoke" && isPeerId(data.value("hash").toString())) {
            revokedInvites.insert(data.value("hash").toString());
            revocations[data.value("hash").toString()].append(key);
        }
        if (operation.value("kind") == "unban" && isPeerId(data.value("peer").toString()) &&
            data.value("peer").toString() != owner)
            unbans[data.value("peer").toString()].append(key);
        if ((operation.value("kind") == "kick" || operation.value("kind") == "remove") &&
            isPeerId(data.value("peer").toString()) &&
            data.value("peer").toString() != owner) {
            removals[data.value("peer").toString()].append(key);
            if (operation.value("kind") == "kick")
                bans[data.value("peer").toString()].append(key);
        }
    }
    // An unban clears only bans it has seen. A concurrent ban still wins.
    const auto banCleared = [&](const QString& ban, const QString& peer, const QString& before) {
        for (const auto& unban : unbans.value(peer))
            if (ancestor(ban, unban) && (before.isEmpty() || ancestor(unban, before)))
                return true;
        return false;
    };
    for (auto it = bans.cbegin(); it != bans.cend(); ++it)
        for (const auto& ban : it.value())
            if (!banCleared(ban, it.key(), {}))
                banned.insert(it.key());
    const auto couldAct = [&](const QString& peer, const QString& operation) {
        for (const auto& removal : removals.value(peer)) {
            if (ancestor(operation, removal))
                continue;
            const auto admission = admissions.value(peer);
            if (admission.isEmpty() || !ancestor(removal, admission) || !ancestor(admission, operation))
                return false;
        }
        for (const auto& departure : departures.value(peer)) {
            if (ancestor(operation, departure))
                continue; // Actions accepted before leaving remain valid history.
            const auto admission = admissions.value(peer);
            if (admission.isEmpty() || !ancestor(departure, admission) ||
                !ancestor(admission, operation))
                return false;
        }
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
            admissions[owner] = key;
            members.insert(owner, {owner,
                                   data.value("memberName").toString(),
                                   data.value("endpoint").toString(),
                                   true,
                                   {},
                                   {}});
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
            bool peerCouldAct = true;
            for (const auto& removal : removals.value(peer))
                if (!ancestor(key, removal) && !ancestor(removal, key))
                    peerCouldAct = false;
            for (const auto& ban : bans.value(peer))
                if (!ancestor(key, ban) && !banCleared(ban, peer, key))
                    peerCouldAct = false;
            bool concurrentDeparture = false;
            for (const auto& departure : departures.value(peer))
                if (!ancestor(departure, key) && !ancestor(key, departure))
                    concurrentDeparture = true;
            // An older admission still belongs to history, but cannot restore a
            // membership after a concurrent or later leave.
            const auto intentId = digest(canonical(intent));
            if (!members.contains(author) || !couldAct(author, key) || !peerCouldAct ||
                concurrentDeparture || acceptedIntents.contains(intentId) ||
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
            acceptedIntents.insert(intentId);
            admissions[peer] = key;
            members.insert(peer, {peer,
                                  intent.value("name").toString(),
                                  intent.value("endpoint").toString(),
                                  false,
                                  {},
                                  {}});
            continue;
        }
        if ((kind == "kick" || kind == "remove") && authorized(key)) {
            const auto peer = data.value("peer").toString();
            if (peer != owner && isPeerId(peer)) {
                members.remove(peer);
                adminGrants_.remove(peer);
            }
            continue;
        }
        if (kind == "leave") {
            if (author != owner) {
                members.remove(author);
                adminGrants_.remove(author);
            }
            continue;
        }
        if (kind == "profile") {
            if (members.contains(author) && couldAct(author, key) &&
                label(data.value("name").toString(), 64) &&
                validEndpoint(data.value("endpoint").toString()) && profileDetails(data)) {
                members[author].name = data.value("name").toString();
                members[author].endpoint = data.value("endpoint").toString();
                if (data.contains("bio"))
                    members[author].bio = data.value("bio").toString();
                if (data.contains("avatar"))
                    members[author].avatar = QByteArray::fromBase64(
                        data.value("avatar").toString().toLatin1());
            }
            continue;
        }
        if (kind == "invite" && members.contains(author) && couldAct(author, key) &&
            isPeerId(data.value("hash").toString()) && data.value("code").toString().size() == 32 &&
            digest(data.value("code").toString().toUtf8()) == data.value("hash").toString()) {
            invites_.insert(data.value("hash").toString(), operation);
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
        else if (kind == "priority" && data.value("peers").isArray()) {
            QStringList next;
            for (const auto& value : data.value("peers").toArray())
                if (members.contains(value.toString()) && !next.contains(value.toString()))
                    next.append(value.toString());
            priority = next;
        }
    }
    for (auto it = departures.cbegin(); it != departures.cend(); ++it) {
        for (const auto& departure : it.value()) {
            if (!ancestor(departure, admissions.value(it.key()))) {
                members.remove(it.key());
                adminGrants_.remove(it.key());
                break;
            }
        }
    }
    for (auto it = removals.cbegin(); it != removals.cend(); ++it) {
        for (const auto& removal : it.value()) {
            if (!ancestor(removal, admissions.value(it.key()))) {
                members.remove(it.key());
                adminGrants_.remove(it.key());
                break;
            }
        }
    }
    for (const auto& peer : banned) {
        members.remove(peer);
        adminGrants_.remove(peer);
    }
}

bool Model::canAdmin(const QString& peer) const {
    return !deleted && members.contains(peer) && (peer == owner || members.value(peer).admin);
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
    const QSet<QString> administrative{"rename", "icon", "channel", "delete-channel",
                                       "revoke", "priority", "kick", "remove", "unban", "admin"};
    require(!deleted, "this community has been deleted");
    require((kind == "leave" && isMember(author.id()) && author.id() != owner) ||
                (kind == "delete" && author.id() == owner) ||
                (kind == "profile" && isMember(author.id())) ||
                (kind == "invite" && isMember(author.id())) ||
                (administrative.contains(kind) && canAdmin(author.id())),
            "only the owner or an admin can make this change");
    require(kind != "admin" || author.id() == owner, "only the owner can assign admins");
    if (kind == "rename")
        require(label(data.value("name").toString()), "invalid community name");
    if (kind == "profile")
        require(label(data.value("name").toString(), 64) &&
                    validEndpoint(data.value("endpoint").toString()) && profileDetails(data),
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
    if (kind == "kick" || kind == "remove")
        require(data.value("peer").toString() != owner &&
                    members.contains(data.value("peer").toString()),
                "cannot remove the owner or an unknown member");
    if (kind == "unban")
        require(banned.contains(data.value("peer").toString()), "this member is not banned");
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
    return !deleted && code.size() == 32 && invites_.contains(hash) && !revokedInvites.contains(hash);
}

QJsonObject joinIntent(const Identity& peer, const Invite& invite, const QString& name,
                       const QString& endpoint, const QString& host, const QString& nonce,
                       const QString& request) {
    QJsonObject intent{{"community", invite.community},
                       {"peer", peer.id()},
                       {"invite", digest(invite.code.toUtf8())},
                       {"name", name},
                       {"endpoint", endpoint},
                       {"host", host},
                       {"nonce", nonce.isEmpty() ? randomCode() : nonce},
                       {"request", request.isEmpty() ? randomCode() : request}};
    intent["proof"] = QString::fromLatin1(
        QMessageAuthenticationCode::hash(canonical(intent), invite.code.toUtf8(),
                                         QCryptographicHash::Sha256)
            .toHex());
    return signedObject(intent, peer, kIntentDomain);
}

bool Model::acceptsIntent(const QString& host, const QJsonObject& intent) const {
    const auto peer = intent.value("peer").toString();
    const auto hash = intent.value("invite").toString();
    if (!isMember(peer)) {
        for (const auto& operation : operations_) {
            if (operation.value("kind") == "admit" &&
                operation.value("data").toObject().value("intent").toObject() == intent)
                return false; // A host cannot replay an old join after a signed leave.
        }
    }
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
    if (deleted)
        return {};
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
