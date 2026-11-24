#include "console/model.h"

#include <algorithm>
#include <cstring>
#include <optional>
#include <stdexcept>

namespace console {
namespace {
constexpr char kOperationDomain[] = "disquisition/community-operation/v1\n";
constexpr char kIntentDomain[] = "disquisition/community-join/v1\n";

void require(bool condition, const char* reason) {
    if (!condition)
        throw std::runtime_error(reason);
}

// QByteArray::fromHex(): non-hex characters are skipped.
std::string fromHexLenient(const std::string& text) {
    std::string digits;
    for (const auto ch : text)
        if ((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f') || (ch >= 'A' && ch <= 'F'))
            digits += ch;
    if (digits.size() % 2)
        digits.insert(digits.begin(), '0');
    std::string bytes;
    fromHex(digits, bytes);
    return bytes;
}

bool profileDetails(const json::Value& data) {
    const auto& bio = data["bio"];
    if (!bio.isUndefined() &&
        (!bio.isString() || json::utf16Length(bio.toString()) > kMaxBioLength ||
         json::hasControl(bio.toString(), true)))
        return false;
    const auto& avatar = data["avatar"];
    if (avatar.isUndefined())
        return true;
    if (!avatar.isString() || avatar.toString().size() > ((kMaxAvatarBytes + 2) / 3) * 4)
        return false;
    std::string decoded;
    return fromBase64Strict(avatar.toString(), decoded) && decoded.size() <= kMaxAvatarBytes;
}

std::string operationId(json::Object operation) {
    operation.erase("signature");
    return digest(json::canonical(json::Value(std::move(operation))));
}

bool inviteProof(json::Object intent, const json::Value& grant) {
    const auto proof = fromHexLenient(intent["proof"].toString());
    intent.erase("proof");
    intent.erase("signature");
    const auto& code = grant["data"]["code"].toString();
    const auto expected = hmacSha256(code, json::canonical(json::Value(std::move(intent))));
    return constantTimeEqual(proof, expected);
}

constexpr char kBase58[] = "123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz";
constexpr int kInviteVersion = 2;
constexpr int kOwnerNotSeed = 0xff;
const char* const kInviteSchemes[] = {"relay", "tcp"};

std::string fromBase58(const std::string& text) {
    std::string bytes; // little-endian
    for (const auto character : text) {
        const char* found = std::strchr(kBase58, character);
        require(character != '\0' && found, "invalid invite encoding");
        int carry = int(found - kBase58);
        for (auto& byte : bytes) {
            carry += static_cast<unsigned char>(byte) * 58;
            byte = char(carry & 0xff);
            carry >>= 8;
        }
        for (; carry; carry >>= 8)
            bytes += char(carry & 0xff);
    }
    for (std::size_t index = 0; index < text.size() && text[index] == '1'; ++index)
        bytes += '\0';
    std::reverse(bytes.begin(), bytes.end());
    return bytes;
}

struct Reader {
    std::string bytes;
    std::size_t at = 0;
    std::string take(std::size_t size) {
        require(size <= bytes.size() - at, "incomplete community invite");
        at += size;
        return bytes.substr(at - size, size);
    }
    int byte() { return static_cast<unsigned char>(take(1)[0]); }
};

std::string uuidText(const std::string& bytes) {
    const auto text = hex(bytes);
    return text.substr(0, 8) + '-' + text.substr(8, 4) + '-' + text.substr(12, 4) + '-' +
           text.substr(16, 4) + '-' + text.substr(20);
}

std::string lower(std::string text) {
    for (auto& ch : text)
        if (ch >= 'A' && ch <= 'Z')
            ch = char(ch - 'A' + 'a');
    return text;
}

// Mirrors QUrl's string form for scheme, host and port.
std::string endpointText(const std::string& scheme, const std::string& host, unsigned port) {
    const auto name = host.find(':') != std::string::npos ? '[' + host + ']' : lower(host);
    return scheme + "://" + name + ':' + std::to_string(port);
}

bool hostCharacter(char ch) {
    return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') ||
           std::strchr("-._~!$&'()*+,;=%", ch) != nullptr;
}

bool parseAuthority(const std::string& authority, std::string& host, long& port) {
    port = -1;
    std::string rest;
    if (!authority.empty() && authority[0] == '[') {
        const auto close = authority.find(']');
        if (close == std::string::npos)
            return false;
        host = authority.substr(1, close - 1);
        if (host.empty() || host.find_first_not_of("0123456789abcdefABCDEF:.") != std::string::npos)
            return false;
        rest = authority.substr(close + 1);
    }
    else {
        const auto colon = authority.rfind(':');
        host = authority.substr(0, colon);
        rest = colon == std::string::npos ? std::string{} : authority.substr(colon);
        if (host.empty() || !std::all_of(host.begin(), host.end(), hostCharacter))
            return false;
    }
    if (rest.empty())
        return true;
    if (rest[0] != ':' || rest.size() < 2 || rest.size() > 6 ||
        rest.find_first_not_of("0123456789", 1) != std::string::npos)
        return false;
    port = std::stol(rest.substr(1));
    return true;
}

bool parseEndpoint(const std::string& endpoint, std::string& scheme, std::string& host, long& port) {
    const auto separator = endpoint.find("://");
    if (separator == std::string::npos)
        return false;
    scheme = endpoint.substr(0, separator);
    auto authority = endpoint.substr(separator + 3);
    if (!authority.empty() && authority.back() == '/')
        authority.pop_back();
    if (authority.find_first_of("/?#@ ") != std::string::npos)
        return false;
    return parseAuthority(authority, host, port);
}

std::optional<Invite> legacyInvite(const std::string& body) {
    if (body.rfind("eyJ", 0) != 0)
        return std::nullopt;
    std::string decoded;
    if (!fromBase64Strict(body, decoded, true))
        return std::nullopt;
    json::Value document;
    try {
        document = json::parse(decoded);
    }
    catch (const std::exception&) {
        return std::nullopt;
    }
    if (!document.isObject() || document["v"].toInt() != 1)
        return std::nullopt;
    return Invite{document["community"].toString(), document["owner"].toString(),
                  document["code"].toString(), document["seeds"].toArray()};
}

Invite compactInvite(const std::string& body) {
    require(body.size() <= 4096, "invalid community invite link");
    Reader reader{fromBase58(body)};
    require(reader.byte() == kInviteVersion, "unsupported or incomplete community invite");
    Invite result;
    result.community = uuidText(reader.take(16));
    result.code = base64Url(reader.take(24));
    std::vector<std::string> endpoints;
    for (std::size_t count = std::size_t(reader.byte()); endpoints.size() < count;) {
        const auto scheme = reader.byte();
        require(scheme < 2, "invalid invite endpoint");
        const auto high = reader.byte();
        const auto port = unsigned(high << 8 | reader.byte());
        const auto host = reader.take(std::size_t(reader.byte()));
        endpoints.push_back(endpointText(kInviteSchemes[scheme], host, port));
    }
    for (std::size_t count = std::size_t(reader.byte()); result.seeds.size() < count;) {
        const auto peer = hex(reader.take(32));
        const auto endpoint = std::size_t(reader.byte());
        require(endpoint < endpoints.size(), "invalid invite endpoint");
        result.seeds.push_back(json::Object{{"peer", peer}, {"endpoint", endpoints[endpoint]}});
    }
    const auto owner = reader.byte();
    require(owner == kOwnerNotSeed || std::size_t(owner) < result.seeds.size(),
            "invalid invite owner");
    result.owner = owner == kOwnerNotSeed ? hex(reader.take(32))
                                          : result.seeds[std::size_t(owner)]["peer"].toString();
    require(reader.at == reader.bytes.size(), "invalid invite data");
    return result;
}
} // namespace

bool label(const std::string& text, std::size_t maximum) {
    return !json::blank(text) && json::utf16Length(text) <= maximum && !json::hasControl(text);
}

bool validEndpoint(const std::string& endpoint) {
    std::string scheme, host;
    long port = -1;
    return endpoint.size() <= 512 && parseEndpoint(endpoint, scheme, host, port) &&
           (scheme == "tcp" || scheme == "relay") && port > 0 && port <= 65535;
}

bool splitEndpoint(const std::string& endpoint, std::string& scheme, std::string& host,
                   unsigned& port) {
    long value = -1;
    if (!validEndpoint(endpoint) || !parseEndpoint(endpoint, scheme, host, value))
        return false;
    port = unsigned(value);
    return true;
}

std::string normalizeRelayEndpoint(const std::string& address) {
    auto input = address;
    const auto first = input.find_first_not_of(" \t\r\n");
    if (first == std::string::npos)
        return {};
    input = input.substr(first, input.find_last_not_of(" \t\r\n") - first + 1);
    if (input.find("://") == std::string::npos)
        input = "relay://" + input;
    std::string scheme, host;
    long port = -1;
    require(parseEndpoint(input, scheme, host, port) && scheme == "relay",
            "Enter a relay hostname with an optional port.");
    const auto result = endpointText("relay", host, unsigned(port == -1 ? 3333 : port));
    require(port != 0 && validEndpoint(result), "Enter a relay hostname with an optional port.");
    return result;
}

Invite Invite::fromLink(const std::string& link) {
    const std::string prefix = "disquisition://join/";
    require(link.rfind(prefix, 0) == 0 && link.size() <= 8192, "invalid community invite link");
    const auto body = link.substr(prefix.size());
    auto legacy = legacyInvite(body);
    const auto result = legacy ? std::move(*legacy) : compactInvite(body);
    require(isUuid(result.community) && isPeerId(result.owner) && result.code.size() == 32 &&
                !result.seeds.empty() && result.seeds.size() <= 8,
            "unsupported or incomplete community invite");
    for (const auto& seed : result.seeds)
        require(isPeerId(seed["peer"].toString()) && validEndpoint(seed["endpoint"].toString()),
                "invalid invite endpoint");
    return result;
}

json::Object Model::toJson() const {
    json::Array operations;
    for (const auto& [key, operation] : operations_)
        operations.push_back(operation);
    return {{"v", 1}, {"id", id}, {"owner", owner}, {"operations", std::move(operations)}};
}

Model Model::load(const json::Value& object, const std::string& expectedId,
                  const std::string& expectedOwner, std::set<std::string> verified) {
    Model model;
    model.verified_ = std::move(verified);
    model.id = object["id"].toString();
    model.owner = object["owner"].toString();
    require(isUuid(model.id) && isPeerId(model.owner) && object["v"].toInt() == 1 &&
                (expectedId.empty() || model.id == expectedId) &&
                (expectedOwner.empty() || model.owner == expectedOwner),
            "community identity mismatch");
    const auto& operations = object["operations"].toArray();
    require(!operations.empty() && operations.size() <= std::size_t(kMaxOperations) &&
                json::canonical(object).size() <= kMaxMetadataBytes,
            "community metadata exceeds its limit");
    for (const auto& operation : operations) {
        require(operation.isObject(), "invalid metadata operation");
        const auto& clock = operation["clock"];
        require(operation["v"].toInt() == 1 && operation["community"].toString() == model.id &&
                    isPeerId(operation["author"].toString()) && operation["parents"].isArray() &&
                    operation["data"].isObject() && clock.isNumber() && clock.toDouble() >= 0 &&
                    clock.toDouble() <= kMaxOperations && clock.toDouble() == clock.toInt(),
                "invalid metadata operation");
        const auto key = operationId(operation.toObject());
        // An operation ID excludes the signature, so the cache key includes it.
        const auto check = key + ':' + operation["signature"].toString();
        if (!model.verified_.count(check)) {
            require(verifyObject(operation.toObject(), operation["author"].toString(),
                                 kOperationDomain),
                    "invalid metadata signature or operation");
            model.verified_.insert(check);
        }
        model.operations_.emplace(key, operation);
    }
    require(model.operations_.size() == operations.size(), "duplicate metadata operations");
    model.rebuild();
    return model;
}

Model Model::fromJson(const json::Value& object, const std::string& expectedId,
                      const std::string& expectedOwner) {
    return load(object, expectedId, expectedOwner, {});
}

bool Model::merge(const json::Value& snapshot) {
    const Model incoming = load(snapshot, id, owner, verified_);
    Model merged = *this;
    merged.verified_ = incoming.verified_;
    for (const auto& [key, operation] : incoming.operations_)
        merged.operations_.emplace(key, operation);
    if (merged.operations_.size() == operations_.size()) {
        verified_ = std::move(merged.verified_);
        return false;
    }
    require(merged.operations_.size() <= std::size_t(kMaxOperations) &&
                json::canonical(json::Value(merged.toJson())).size() <= kMaxMetadataBytes,
            "community metadata exceeds its limit");
    merged.rebuild();
    *this = std::move(merged);
    return true;
}

bool Model::ancestor(const std::string& older, const std::string& newer) const {
    if (older == newer)
        return true;
    std::set<std::string> visited;
    std::vector<std::string> pending{newer};
    while (!pending.empty()) {
        const auto current = pending.back();
        pending.pop_back();
        if (!visited.insert(current).second)
            continue;
        const auto found = operations_.find(current);
        if (found == operations_.end())
            continue;
        for (const auto& parent : found->second["parents"].toArray()) {
            if (parent.toString() == older)
                return true;
            pending.push_back(parent.toString());
        }
    }
    return false;
}

bool Model::validIntent(const json::Object& intent) const {
    const json::Value value(intent);
    if (value["community"].toString() != id || !isPeerId(value["peer"].toString()) ||
        !label(value["name"].toString(), 64) || !validEndpoint(value["endpoint"].toString()) ||
        !isPeerId(value["invite"].toString()))
        return false;
    const auto check = "intent:" + digest(json::canonical(value));
    if (verified_.count(check))
        return true;
    if (!verifyObject(intent, value["peer"].toString(), kIntentDomain))
        return false;
    verified_.insert(check);
    return true;
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
    name.clear();
    clock_ = 0;
    std::vector<std::string> ordered;
    for (const auto& entry : operations_)
        ordered.push_back(entry.first);
    std::sort(ordered.begin(), ordered.end(), [this](const auto& a, const auto& b) {
        const int ca = operations_.at(a)["clock"].toInt();
        const int cb = operations_.at(b)["clock"].toInt();
        return ca == cb ? a < b : ca < cb;
    });
    const auto operation = [this](const std::string& key) -> const json::Value& {
        static const json::Value missing = json::Value::undefined();
        const auto found = operations_.find(key);
        return found == operations_.end() ? missing : found->second;
    };
    int roots = 0;
    std::set<std::string> nonHeads;
    std::vector<std::string> ownerRoles;
    for (const auto& key : ordered) {
        const auto& current = operation(key);
        const auto& parents = current["parents"].toArray();
        const int clock = current["clock"].toInt();
        if (current["kind"].toString() == "create") {
            require(parents.empty() && clock == 0 && current["author"].toString() == owner,
                    "invalid community origin");
            ++roots;
        }
        else {
            require(!parents.empty(), "metadata operation has no ancestry");
            int parentClock = -1;
            std::set<std::string> unique;
            for (const auto& value : parents) {
                const auto& parent = value.toString();
                require(operations_.count(parent) && !unique.count(parent) &&
                            operation(parent)["clock"].toInt() < clock,
                        "invalid metadata ancestry");
                unique.insert(parent);
                parentClock = std::max(parentClock, operation(parent)["clock"].toInt());
                nonHeads.insert(parent);
            }
            require(clock == parentClock + 1, "invalid metadata clock");
        }
        clock_ = std::max(clock_, clock);
        heads_.insert(key);
        if (current["kind"].toString() == "admin" && current["author"].toString() == owner)
            ownerRoles.push_back(key);
    }
    require(roots == 1, "community must have exactly one origin");
    for (const auto& key : nonHeads)
        heads_.erase(key);
    // A revoked grant cannot authorize concurrent or later edits. Edits that
    // causally preceded its revocation remain part of the accepted history.
    const auto authorized = [&](const std::string& key) {
        const auto& current = operation(key);
        const auto& author = current["author"].toString();
        if (author == owner)
            return true;
        const auto& grantId = current["authority"].toString();
        if (!operations_.count(grantId))
            return false;
        const auto& grant = operation(grantId);
        const auto& data = grant["data"];
        if (grant["author"].toString() != owner || grant["kind"].toString() != "admin" ||
            data["peer"].toString() != author || !data["enabled"].toBool() ||
            !ancestor(grantId, key))
            return false;
        for (const auto& roleId : ownerRoles) {
            const auto& role = operation(roleId)["data"];
            if (role["peer"].toString() == author && roleId != grantId &&
                !ancestor(roleId, grantId) && !ancestor(key, roleId))
                return false;
        }
        return true;
    };
    std::map<std::string, std::vector<std::string>> revocations, removals, bans, unbans, departures;
    std::map<std::string, std::string> admissions;
    std::set<std::string> acceptedIntents;
    const auto valueOf = [](const std::map<std::string, std::vector<std::string>>& map,
                            const std::string& key) -> const std::vector<std::string>& {
        static const std::vector<std::string> none;
        const auto found = map.find(key);
        return found == map.end() ? none : found->second;
    };
    const auto admission = [&](const std::string& peer) {
        const auto found = admissions.find(peer);
        return found == admissions.end() ? std::string{} : found->second;
    };
    for (const auto& key : ordered) {
        const auto& current = operation(key);
        const auto& data = current["data"];
        const auto& author = current["author"].toString();
        const auto& kind = current["kind"].toString();
        if (kind == "leave" && author != owner)
            departures[author].push_back(key);
        if (kind == "delete" && author == owner)
            deleted = true;
        if (!authorized(key))
            continue;
        if (kind == "revoke" && isPeerId(data["hash"].toString())) {
            revokedInvites.insert(data["hash"].toString());
            revocations[data["hash"].toString()].push_back(key);
        }
        if (kind == "unban" && isPeerId(data["peer"].toString()) && data["peer"].toString() != owner)
            unbans[data["peer"].toString()].push_back(key);
        if ((kind == "kick" || kind == "remove") && isPeerId(data["peer"].toString()) &&
            data["peer"].toString() != owner) {
            removals[data["peer"].toString()].push_back(key);
            if (kind == "kick")
                bans[data["peer"].toString()].push_back(key);
        }
    }
    // An unban clears only bans it has seen. A concurrent ban still wins.
    const auto banCleared = [&](const std::string& ban, const std::string& peer,
                                const std::string& before) {
        for (const auto& unban : valueOf(unbans, peer))
            if (ancestor(ban, unban) && (before.empty() || ancestor(unban, before)))
                return true;
        return false;
    };
    for (const auto& [peer, list] : bans)
        for (const auto& ban : list)
            if (!banCleared(ban, peer, {}))
                banned.insert(peer);
    const auto couldAct = [&](const std::string& peer, const std::string& key) {
        for (const auto& removal : valueOf(removals, peer)) {
            if (ancestor(key, removal))
                continue;
            const auto admitted = admission(peer);
            if (admitted.empty() || !ancestor(removal, admitted) || !ancestor(admitted, key))
                return false;
        }
        for (const auto& departure : valueOf(departures, peer)) {
            if (ancestor(key, departure))
                continue; // Actions accepted before leaving remain valid history.
            const auto admitted = admission(peer);
            if (admitted.empty() || !ancestor(departure, admitted) || !ancestor(admitted, key))
                return false;
        }
        return true;
    };
    for (const auto& key : ordered) {
        const auto& current = operation(key);
        const auto& kind = current["kind"].toString();
        const auto& author = current["author"].toString();
        const auto& data = current["data"];
        if (kind == "create") {
            require(label(data["name"].toString()) && label(data["memberName"].toString(), 64) &&
                        validEndpoint(data["endpoint"].toString()),
                    "invalid community origin data");
            name = data["name"].toString();
            admissions[owner] = key;
            members[owner] = {owner, data["memberName"].toString(), data["endpoint"].toString(),
                              true};
            require(data["channels"].toArray().size() <= 128, "too many channels");
            for (const auto& channel : data["channels"].toArray()) {
                const auto& channelId = channel["id"].toString();
                const auto& type = channel["type"].toString();
                require(isUuid(channelId) && label(channel["name"].toString()) &&
                            (type == "text" || type == "voice"),
                        "invalid initial channel");
                channels[channelId] = {channelId, channel["name"].toString(), type, {}};
            }
            continue;
        }
        if (kind == "admit") {
            const auto& intent = data["intent"];
            const auto& peer = intent["peer"].toString();
            bool peerCouldAct = true;
            for (const auto& removal : valueOf(removals, peer))
                if (!ancestor(key, removal) && !ancestor(removal, key))
                    peerCouldAct = false;
            for (const auto& ban : valueOf(bans, peer))
                if (!ancestor(key, ban) && !banCleared(ban, peer, key))
                    peerCouldAct = false;
            bool concurrentDeparture = false;
            for (const auto& departure : valueOf(departures, peer))
                if (!ancestor(departure, key) && !ancestor(key, departure))
                    concurrentDeparture = true;
            // An older admission still belongs to history, but cannot restore a
            // membership after a concurrent or later leave.
            const auto intentId = digest(json::canonical(intent));
            if (!members.count(author) || !couldAct(author, key) || !peerCouldAct ||
                concurrentDeparture || acceptedIntents.count(intentId) ||
                !validIntent(intent.toObject()) || members.count(peer) ||
                members.size() >= kMaxMembers)
                continue;
            const auto& hash = intent["invite"].toString();
            const auto invite = invites_.find(hash);
            if (invite == invites_.end() || !inviteProof(intent.toObject(), invite->second) ||
                (!intent["host"].toString().empty() && intent["host"].toString() != author))
                continue;
            bool revoked = false;
            for (const auto& revocation : valueOf(revocations, hash))
                if (!ancestor(key, revocation))
                    revoked = true;
            if (revoked)
                continue;
            acceptedIntents.insert(intentId);
            admissions[peer] = key;
            members[peer] = {peer, intent["name"].toString(), intent["endpoint"].toString(), false};
            continue;
        }
        if ((kind == "kick" || kind == "remove") && authorized(key)) {
            const auto& peer = data["peer"].toString();
            if (peer != owner && isPeerId(peer)) {
                members.erase(peer);
                adminGrants_.erase(peer);
            }
            continue;
        }
        if (kind == "leave") {
            if (author != owner) {
                members.erase(author);
                adminGrants_.erase(author);
            }
            continue;
        }
        if (kind == "profile") {
            if (members.count(author) && couldAct(author, key) && label(data["name"].toString(), 64) &&
                validEndpoint(data["endpoint"].toString()) && profileDetails(data)) {
                members[author].name = data["name"].toString();
                members[author].endpoint = data["endpoint"].toString();
            }
            continue;
        }
        if (kind == "invite" && members.count(author) && couldAct(author, key) &&
            isPeerId(data["hash"].toString()) && data["code"].toString().size() == 32 &&
            digest(data["code"].toString()) == data["hash"].toString()) {
            invites_[data["hash"].toString()] = current;
            continue;
        }
        if (!members.count(author) || !couldAct(author, key) || !authorized(key))
            continue;
        if (kind == "admin" && author == owner) {
            const auto& peer = data["peer"].toString();
            if (peer != owner && members.count(peer) && data["enabled"].isBool()) {
                members[peer].admin = data["enabled"].toBool();
                if (members[peer].admin)
                    adminGrants_[peer] = key;
                else
                    adminGrants_.erase(peer);
            }
        }
        else if (kind == "rename" && label(data["name"].toString())) {
            name = data["name"].toString();
        }
        else if (kind == "channel") {
            const auto& channelId = data["id"].toString();
            const auto& type = data["type"].toString();
            if (isUuid(channelId) && label(data["name"].toString()) &&
                (type == "text" || type == "voice") &&
                json::utf16Length(data["topic"].toString()) <= 512 &&
                (channels.count(channelId) || channels.size() < 128))
                channels[channelId] = {channelId, data["name"].toString(), type,
                                       data["topic"].toString()};
        }
        else if (kind == "delete-channel") {
            channels.erase(data["id"].toString());
        }
        else if (kind == "priority" && data["peers"].isArray()) {
            std::vector<std::string> next;
            for (const auto& value : data["peers"].toArray())
                if (members.count(value.toString()) &&
                    std::find(next.begin(), next.end(), value.toString()) == next.end())
                    next.push_back(value.toString());
            priority = next;
        }
    }
    for (const auto& [peer, list] : departures) {
        for (const auto& departure : list) {
            if (!ancestor(departure, admission(peer))) {
                members.erase(peer);
                adminGrants_.erase(peer);
                break;
            }
        }
    }
    for (const auto& [peer, list] : removals) {
        for (const auto& removal : list) {
            if (!ancestor(removal, admission(peer))) {
                members.erase(peer);
                adminGrants_.erase(peer);
                break;
            }
        }
    }
    for (const auto& peer : banned) {
        members.erase(peer);
        adminGrants_.erase(peer);
    }
}

bool Model::canAdmin(const std::string& peer) const {
    const auto found = members.find(peer);
    return !deleted && found != members.end() && (peer == owner || found->second.admin);
}

json::Object Model::append(const Identity& author, const std::string& kind, json::Object data) {
    json::Array parents;
    for (const auto& head : heads_) // std::set is sorted, like QStringList::sort()
        parents.push_back(head);
    json::Object operation{{"v", 1},
                           {"community", id},
                           {"author", author.id()},
                           {"clock", clock_ + 1},
                           {"parents", std::move(parents)},
                           {"kind", kind},
                           {"data", std::move(data)}};
    const auto grant = adminGrants_.find(author.id());
    if (author.id() != owner && grant != adminGrants_.end())
        operation["authority"] = grant->second;
    operation = signedObject(std::move(operation), author, kOperationDomain);
    Model next = *this;
    const auto key = operationId(operation);
    next.verified_.insert(key + ':' + json::Value(operation)["signature"].toString());
    next.operations_.emplace(key, operation);
    require(next.operations_.size() <= std::size_t(kMaxOperations) &&
                json::canonical(json::Value(next.toJson())).size() <= kMaxMetadataBytes,
            "community metadata exceeds its limit");
    next.rebuild();
    *this = std::move(next);
    return operation;
}

json::Object Model::change(const Identity& author, const std::string& kind, json::Object data) {
    static const std::set<std::string> administrative{
        "rename", "icon", "channel", "delete-channel", "revoke",
        "priority", "kick", "remove", "unban", "admin"};
    const json::Value value(data);
    require(!deleted, "this community has been deleted");
    require((kind == "leave" && isMember(author.id()) && author.id() != owner) ||
                (kind == "delete" && author.id() == owner) ||
                (kind == "profile" && isMember(author.id())) ||
                (kind == "invite" && isMember(author.id())) ||
                (administrative.count(kind) && canAdmin(author.id())),
            "only the owner or an admin can make this change");
    require(kind != "admin" || author.id() == owner, "only the owner can assign admins");
    if (kind == "profile")
        require(label(value["name"].toString(), 64) && validEndpoint(value["endpoint"].toString()) &&
                    profileDetails(value),
                "invalid member profile");
    // Consoles only publish their profile or leave; the desktop validates
    // administrative edits before signing them.
    require(kind == "profile" || kind == "leave", "unsupported community change");
    return append(author, kind, std::move(data));
}

bool Model::acceptsIntent(const std::string& host, const json::Object& intent) const {
    const json::Value value(intent);
    const auto& peer = value["peer"].toString();
    const auto& hash = value["invite"].toString();
    if (!isMember(peer)) {
        for (const auto& [key, operation] : operations_) {
            if (operation["kind"].toString() == "admit" && operation["data"]["intent"] == value)
                return false; // A host cannot replay an old join after a signed leave.
        }
    }
    const auto invite = invites_.find(hash);
    return isMember(host) && validIntent(intent) && invite != invites_.end() &&
           !revokedInvites.count(hash) && inviteProof(intent, invite->second) &&
           (value["host"].toString().empty() || value["host"].toString() == host) &&
           !banned.count(peer) && (isMember(peer) || members.size() < kMaxMembers);
}

json::Object Model::admit(const Identity& hostIdentity, const json::Object& intent) {
    require(acceptsIntent(hostIdentity.id(), intent) &&
                !isMember(json::Value(intent)["peer"].toString()),
            "join rejected");
    return append(hostIdentity, "admit", {{"intent", intent}});
}

std::string Model::revision() const {
    std::string keys;
    for (const auto& [key, operation] : operations_) {
        if (!keys.empty())
            keys += ',';
        keys += key;
    }
    return digest(keys);
}

std::string Model::host(const std::set<std::string>& online) const {
    if (deleted)
        return {};
    for (const auto& peer : priority)
        if (members.count(peer) && online.count(peer))
            return peer;
    if (online.count(owner))
        return owner;
    for (const auto& [peer, member] : members)
        if (online.count(peer))
            return peer;
    return {};
}

json::Object joinIntent(const Identity& peer, const Invite& invite, const std::string& name,
                        const std::string& endpoint, const std::string& host,
                        const std::string& nonce, const std::string& request) {
    json::Object intent{{"community", invite.community},
                        {"peer", peer.id()},
                        {"invite", digest(invite.code)},
                        {"name", name},
                        {"endpoint", endpoint},
                        {"host", host},
                        {"nonce", nonce.empty() ? randomCode() : nonce},
                        {"request", request.empty() ? randomCode() : request}};
    intent["proof"] = hex(hmacSha256(invite.code, json::canonical(json::Value(intent))));
    return signedObject(std::move(intent), peer, kIntentDomain);
}

} // namespace console
