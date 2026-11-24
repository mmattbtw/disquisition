#include "console/client.h"

#include <algorithm>
#include <stdexcept>

namespace console {
namespace {
// Identifies community routing on the relay's TCP port (community/wire.h).
constexpr char kRelayPrefix[] = "DQC1\n";
constexpr std::size_t kRelayPrefixSize = sizeof(kRelayPrefix) - 1;
constexpr char kRelayDomain[] = "disquisition/relay/v1\n";
constexpr std::size_t kMaxPacketBytes = 256 * 1024;
// Consoles have far less memory than desktops. These bounds are lower than
// the desktop's 256 links and 16 MiB send backlog, and limit concurrent
// handshakes so an original 3DS stays responsive.
constexpr std::size_t kMaxLinks = 32;
constexpr std::size_t kMaxHandshakes = 4;
constexpr std::size_t kMaxBacklog = 4 * 1024 * 1024;
constexpr std::size_t kMaxSnapshotBuffer = 2 * kMaxMetadataBytes;
constexpr std::size_t kPaceBytes = 64 * 1024;

std::string frame(const json::Value& message) {
    const auto payload = json::write(message);
    if (payload.empty() || payload.size() > kMaxPacketBytes)
        throw std::runtime_error("community frame is too large");
    std::string result(4, '\0');
    const auto size = std::uint32_t(payload.size());
    result[0] = char(size >> 24);
    result[1] = char(size >> 16);
    result[2] = char(size >> 8);
    result[3] = char(size);
    return result + payload;
}

bool takeFrame(std::string& bytes, json::Value& out, std::size_t limit = kMaxPacketBytes) {
    if (bytes.size() < 4)
        return false;
    const auto size = std::uint32_t(static_cast<unsigned char>(bytes[0])) << 24 |
                      std::uint32_t(static_cast<unsigned char>(bytes[1])) << 16 |
                      std::uint32_t(static_cast<unsigned char>(bytes[2])) << 8 |
                      std::uint32_t(static_cast<unsigned char>(bytes[3]));
    if (size == 0 || size > limit)
        throw std::runtime_error("invalid community frame size");
    if (bytes.size() < std::size_t(size) + 4)
        return false;
    out = json::parse(std::string_view(bytes).substr(4, size));
    if (!out.isObject())
        throw std::runtime_error("invalid community frame");
    bytes.erase(0, std::size_t(size) + 4);
    return true;
}

std::string authority(const std::string& endpoint) {
    const auto separator = endpoint.find("://");
    return separator == std::string::npos ? endpoint : endpoint.substr(separator + 3);
}

json::Object inviteObject(const Invite& invite) {
    return {{"community", invite.community},
            {"owner", invite.owner},
            {"code", invite.code},
            {"seeds", invite.seeds}};
}
} // namespace

struct Client::Link {
    Stream stream;
    std::string peer;
    std::string expected;
    std::string discover;
    std::string route;
    std::string relayAction;
    std::string ticket;
    std::string nonce = randomCode();
    std::string remoteNonce;
    bool outgoing = false;
    bool tls = false;
    bool prefixSent = false;
    bool hello = false;
    bool dead = false;
    std::uint64_t started = 0;
    std::uint64_t touched = 0;
    std::set<std::string> claimed;
    std::set<std::string> allowed;
    std::set<std::string> ready;
    std::map<std::string, std::uint64_t> synchronizing;
    std::map<std::string, json::Object> admissions;
    struct Snapshot {
        std::string owner;
        json::Array operations;
        int count = 0;
        std::size_t bytes = 0;
        std::uint64_t started = 0;
        bool complete = false;
    };
    std::map<std::string, Snapshot> snapshots;
    // Frames wait here, in order, while the socket is behind. Snapshot
    // operations are framed only when sent, so a large history never needs
    // to be buffered at once.
    struct Outgoing {
        std::string bytes;
        std::string community;
        json::Value operation;
    };
    std::deque<Outgoing> outbox;
    std::size_t outboxBytes = 0;
    std::size_t outboxOperations = 0;
    bool closing = false; // close once queued frames are written
};

Client::Client(Storage& storage, Clock& clock) : storage_(storage), clock_(clock) {}

Client::~Client() {
    stop();
}

void Client::load() {
    std::string bytes;
    if (storage_.read("identity.key", bytes, 32))
        identity_ = Identity::fromSecret(bytes);
    else {
        identity_ = Identity::generate();
        storage_.write("identity.key", identity_->secret());
    }
    models_.clear();
    for (const auto& file : storage_.list(".community.json")) {
        if (!storage_.read(file, bytes, kMaxMetadataBytes))
            continue;
        auto model = Model::fromJson(json::parse(bytes));
        if (file != model.id + ".community.json")
            throw std::runtime_error("community file identity mismatch");
        models_.emplace(model.id, std::move(model));
    }
    pending_.clear();
    if (storage_.read("pending.json", bytes, 64 * 1024)) {
        const auto saved = json::parse(bytes);
        for (const auto& value : saved.toArray()) {
            Invite invite{value["community"].toString(), value["owner"].toString(),
                          value["code"].toString(), value["seeds"].toArray()};
            if (isUuid(invite.community) && isPeerId(invite.owner) && !invite.seeds.empty() &&
                !models_.count(invite.community))
                pending_[invite.community] = std::move(invite);
        }
    }
}

bool Client::start(const std::string& displayName, const std::string& relay) {
    if (running_)
        return true;
    if (!identity_)
        load();
    name_ = displayName;
    if (!label(name_, 64)) {
        notify(Event::Kind::Error, {}, "Enter a name of at most 64 characters.");
        return false;
    }
    try {
        relayEndpoint_ = normalizeRelayEndpoint(relay);
        if (relayEndpoint_.empty())
            throw std::runtime_error("Enter a relay hostname with an optional port.");
        if (!credentials_)
            credentials_ = std::make_unique<Credentials>(*identity_);
    }
    catch (const std::exception& error) {
        notify(Event::Kind::Error, {}, error.what());
        return false;
    }
    // Consoles always use a relay: they open no inbound port.
    endpoint_ = relayEndpoint_;
    relayError_.clear();
    running_ = true;
    recoveringUntil_ = now() + 2000;
    try {
        std::vector<std::string> ids;
        for (const auto& entry : models_)
            ids.push_back(entry.first);
        for (const auto& id : ids)
            publishProfile(id);
    }
    catch (const std::exception& error) {
        running_ = false;
        notify(Event::Kind::Error, {}, error.what());
        return false;
    }
    tick();
    notify(Event::Kind::Changed);
    return true;
}

void Client::stop() {
    running_ = false;
    relay_.reset();
    relayNonce_.clear();
    relayRegistered_ = false;
    relayDue_ = 0;
    links_.clear();
    peers_.clear();
    joining_.clear();
    joinStarted_.clear();
    presence_.clear();
    mine_.clear();
}

std::vector<Event> Client::takeEvents() {
    std::vector<Event> result;
    result.swap(events_);
    return result;
}

void Client::notify(Event::Kind kind, const std::string& community, const std::string& message) {
    Event event;
    event.kind = kind;
    event.community = community;
    event.message = message;
    // Coalesce redundant change notifications.
    if (kind == Event::Kind::Changed && !events_.empty() &&
        events_.back().kind == Event::Kind::Changed)
        return;
    events_.push_back(std::move(event));
}

std::set<std::string> Client::pendingCommunities() const {
    std::set<std::string> result;
    for (const auto& entry : pending_)
        result.insert(entry.first);
    return result;
}

void Client::save(Model model) {
    // Never advertise an update we haven't durably saved.
    storage_.write(model.id + ".community.json", json::canonical(json::Value(model.toJson())));
    const auto id = model.id;
    models_[id] = std::move(model);
    if (!models_[id].isMember(peerId())) {
        mine_.erase(id);
        presence_.erase(id);
    }
    registerRelay();
    notify(Event::Kind::Changed);
}

void Client::persistPending() {
    json::Array array;
    for (const auto& entry : pending_)
        array.push_back(inviteObject(entry.second));
    storage_.write("pending.json", json::write(json::Value(std::move(array))));
}

void Client::joinCommunity(const std::string& link) {
    if (!running_)
        throw std::runtime_error("connect before joining a community");
    auto trimmed = link;
    trimmed.erase(0, trimmed.find_first_not_of(" \t\r\n"));
    trimmed.erase(trimmed.find_last_not_of(" \t\r\n") + 1);
    const auto invite = Invite::fromLink(trimmed);
    const auto existing = models_.find(invite.community);
    if (existing != models_.end() && existing->second.isMember(peerId())) {
        notify(Event::Kind::Joined, invite.community);
        return;
    }
    if (pending_.size() >= 16)
        throw std::runtime_error("too many pending invitations");
    pending_[invite.community] = invite;
    joining_.erase(invite.community);
    joinStarted_.erase(invite.community);
    persistPending();
    retries_.clear();
    tick();
}

void Client::leaveCommunity(const std::string& cid) {
    const auto found = models_.find(cid);
    if (found == models_.end()) {
        if (pending_.erase(cid)) {
            joining_.erase(cid);
            joinStarted_.erase(cid);
            persistPending();
            notify(Event::Kind::Changed);
        }
        return;
    }
    if (mine_.count(cid) && !mine_[cid].voiceChannel.empty())
        setVoice(cid, {});
    auto model = found->second;
    model.change(*identity_, "leave", {});
    save(std::move(model));
    broadcastState(cid);
}

void Client::publishProfile(const std::string& cid) {
    const auto found = models_.find(cid);
    if (found == models_.end() || !found->second.isMember(peerId()))
        return;
    const auto& member = found->second.members.at(peerId());
    if (member.name == name_ && member.endpoint == endpoint_)
        return;
    auto model = found->second;
    model.change(*identity_, "profile",
                 {{"name", name_}, {"bio", ""}, {"avatar", ""}, {"endpoint", endpoint_}});
    save(std::move(model));
    broadcastState(cid);
}

Client::Link* Client::makeLink(bool outgoing, const std::string& expected) {
    auto link = std::make_unique<Link>();
    link->outgoing = outgoing;
    link->expected = expected;
    link->started = link->touched = now();
    links_.push_back(std::move(link));
    return links_.back().get();
}

void Client::dropLink(Link* link, const std::string& reason) {
    if (link->dead)
        return;
    link->dead = true;
    const auto found = peers_.find(link->peer);
    if (found != peers_.end() && found->second == link) {
        peers_.erase(found);
        for (auto& group : presence_)
            group.second.erase(link->peer);
    }
    for (auto it = joining_.begin(); it != joining_.end();) {
        if (it->second == link) {
            joinStarted_.erase(it->first);
            it = joining_.erase(it);
        }
        else
            ++it;
    }
    link->stream.close(reason);
    notify(Event::Kind::Changed);
}

void Client::dial(const std::string& peer, const std::string& endpoint,
                  const std::string& discover) {
    if (!validEndpoint(endpoint) || peer == peerId() || links_.size() >= kMaxLinks)
        return;
    const auto key = peer + ':' + endpoint + ':' + discover;
    if (peers_.count(peer))
        return;
    std::size_t handshakes = 0;
    for (const auto& link : links_) {
        if (link->dead)
            continue;
        if (link->route == key)
            return;
        handshakes += link->tls ? 0 : 1;
    }
    if (handshakes >= kMaxHandshakes)
        return;
    auto& retry = retries_[key];
    if (retry.due > now())
        return;
    retry.failures = std::min(6, retry.failures + 1);
    unsigned jitter = 0;
    randomBytes(&jitter, sizeof(jitter));
    retry.due = now() + std::uint64_t(std::min(30000, 500 * (1 << retry.failures))) + jitter % 300;
    std::string scheme, host;
    unsigned port = 0;
    if (!splitEndpoint(endpoint, scheme, host, port))
        return;
    auto* link = makeLink(true, peer);
    link->route = key;
    link->discover = discover;
    if (scheme == "relay")
        link->relayAction = "dial";
    if (!link->stream.connect(host, port, now()))
        dropLink(link);
}

void Client::send(Link* link, const json::Object& message) {
    if (link->dead || !link->tls)
        return;
    auto bytes = frame(json::Value(message));
    if (link->stream.backlog() + link->outboxBytes + bytes.size() > kMaxBacklog)
        throw std::runtime_error("community connection is backlogged");
    if (!link->outbox.empty() || link->stream.backlog() >= kPaceBytes) {
        link->outboxBytes += bytes.size();
        link->outbox.push_back({std::move(bytes), {}, {}});
        return;
    }
    link->stream.write(bytes);
}

void Client::pump(Link* link) {
    while (!link->outbox.empty() && link->stream.backlog() < kPaceBytes &&
           link->stream.state() == Stream::State::Secure) {
        auto& next = link->outbox.front();
        if (next.bytes.empty()) {
            next.bytes = frame(json::Value(json::Object{{"type", "state-operation"},
                                                        {"community", next.community},
                                                        {"operation", next.operation}}));
            --link->outboxOperations;
        }
        else
            link->outboxBytes -= next.bytes.size();
        link->stream.write(next.bytes);
        link->outbox.pop_front();
    }
    if (link->closing && link->outbox.empty())
        link->stream.finish();
}

void Client::pollLink(Link* link) {
    try {
        auto& stream = link->stream;
        const bool open = stream.poll(now());
        if (!open && !link->tls) {
            dropLink(link, stream.error());
            return;
        }
        if (stream.state() == Stream::State::Plain) {
            if (link->relayAction.empty()) {
                stream.startTls(*credentials_, link->outgoing); // direct tcp:// endpoint
                return;
            }
            if (!link->prefixSent) {
                stream.writePlain(std::string(kRelayPrefix, kRelayPrefixSize));
                link->prefixSent = true;
            }
            json::Value response;
            while (takeFrame(stream.plain(), response, 64 * 1024)) {
                link->touched = now();
                const auto& type = response["type"].toString();
                if (type == "challenge") {
                    json::Object request{{"type", link->relayAction},
                                         {"peer", peerId()},
                                         {"nonce", response["nonce"]},
                                         {"target", link->expected},
                                         {"community", link->discover},
                                         {"ticket", link->ticket}};
                    stream.writePlain(
                        frame(json::Value(signedObject(request, *identity_, kRelayDomain))));
                }
                else if (type == "ready") {
                    if (link->relayAction == "dial" && !link->discover.empty())
                        link->expected = response["peer"].toString();
                    const bool client = link->relayAction == "dial";
                    link->relayAction.clear();
                    stream.startTls(*credentials_, client);
                    return;
                }
                else {
                    dropLink(link);
                    return;
                }
            }
            return;
        }
        if (open && stream.state() != Stream::State::Secure)
            return;
        if (!link->tls)
            encrypted(link);
        // Frames that arrived just before the peer closed still count, such
        // as a host's redirect or rejection.
        json::Value message;
        while (!link->dead && takeFrame(stream.received(), message)) {
            link->touched = now();
            handle(link, message);
        }
        if (!link->dead)
            pump(link);
        if (!open && !link->dead)
            dropLink(link, stream.error());
    }
    catch (const std::exception& error) {
        notify(Event::Kind::Error, {}, error.what());
        dropLink(link, error.what());
    }
}

void Client::encrypted(Link* link) {
    const auto& peer = link->stream.peer();
    if (peer.empty() || peer == peerId() || (!link->expected.empty() && peer != link->expected)) {
        notify(Event::Kind::Error, {},
             "Peer identity certificate is missing, invalid, or does not match the requested peer.");
        dropLink(link);
        return;
    }
    link->tls = true;
    link->peer = peer;
    const auto existing = peers_.find(peer);
    if (existing != peers_.end()) {
        // Both sides select the connection initiated by the smaller peer ID.
        const bool preferOutgoing = peerId() < peer;
        if (existing->second->outgoing == preferOutgoing || link->outgoing != preferOutgoing) {
            dropLink(link);
            return;
        }
        auto* previous = existing->second;
        peers_[peer] = link;
        dropLink(previous);
    }
    else
        peers_[peer] = link;
    retries_.erase(link->route);
    json::Array communities;
    for (const auto& [id, model] : models_)
        if (model.isMember(peerId()) && model.isMember(peer))
            communities.push_back(json::Object{{"id", id}, {"revision", model.revision()}});
    send(link, {{"type", "hello"},
                {"protocol", "disquisition-community"},
                {"v", 1},
                {"peer", peerId()},
                {"nonce", link->nonce},
                {"communities", std::move(communities)}});
}

bool Client::writable(const Link* link, const std::string& cid) const {
    const auto found = models_.find(cid);
    return found != models_.end() && found->second.isMember(peerId()) &&
           found->second.isMember(link->peer);
}

json::Object Client::presenceObject(const std::string& cid, const Presence& presence) const {
    return {{"type", "presence"},
            {"community", cid},
            {"channel", presence.voiceChannel},
            {"muted", presence.muted},
            {"deafened", presence.deafened},
            {"sharing", false},
            {"camera", false}};
}

void Client::mergeSnapshot(Link* link, const std::string& cid, const json::Value& object) {
    // The desktop validates on a thread pool. Consoles validate inline on the
    // worker thread, which owns no UI, so no other state can change meanwhile.
    const auto base = models_.find(cid);
    Model candidate;
    bool hasChanges = true;
    if (base == models_.end())
        candidate = Model::fromJson(object, cid, object["owner"].toString());
    else {
        candidate = base->second;
        hasChanges = candidate.merge(object);
    }
    link->snapshots.erase(cid);
    if (!candidate.isMember(link->peer) && !link->allowed.count(cid))
        throw std::runtime_error("snapshot sender is not a member");
    const bool joining = pending_.count(cid) != 0;
    const bool admitted = candidate.isMember(peerId());
    // A retained community can receive an older snapshot while rejoining.
    // Merge its history, but keep the join pending until membership is proven.
    if (joining && !admitted && !models_.count(cid))
        throw std::runtime_error("admission proof was not accepted");
    if (hasChanges)
        save(std::move(candidate));
    if (joining && admitted) {
        pending_.erase(cid);
        joining_.erase(cid);
        joinStarted_.erase(cid);
        persistPending();
        notify(Event::Kind::Joined, cid);
    }
    link->claimed.insert(cid);
    if (writable(link, cid)) {
        link->allowed.insert(cid);
        link->ready.insert(cid);
        link->synchronizing.erase(cid);
        send(link, {{"type", "synced"}, {"community", cid}, {"revision", models_[cid].revision()}});
        send(link, presenceObject(cid, mine_[cid]));
    }
    else {
        link->allowed.erase(cid);
        link->ready.erase(cid);
        link->synchronizing.erase(cid);
    }
    if (hasChanges)
        broadcastState(cid);
    publishProfile(cid);
    notify(Event::Kind::Changed);
}

void Client::authorize(Link* link, const std::string& cid) {
    if (!writable(link, cid))
        return;
    link->allowed.insert(cid);
    if (!link->ready.count(cid))
        link->synchronizing[cid] = now();
    send(link, {{"type", "offer"}, {"community", cid}, {"revision", models_[cid].revision()}});
}

void Client::offer(Link* link) {
    if (!link->hello)
        return;
    for (const auto& entry : models_)
        authorize(link, entry.first);
    for (const auto& [cid, invite] : pending_) {
        const auto joining = joining_.find(cid);
        if (joining != joining_.end() && joining->second == link)
            send(link, {{"type", "join"},
                        {"community", cid},
                        {"intent", joinIntent(*identity_, invite, name_, endpoint_, link->peer,
                                              link->remoteNonce)}});
    }
}

void Client::sendState(Link* link, const std::string& cid) {
    if (!link->hello || !link->allowed.count(cid))
        return;
    const auto snapshot = json::Value(models_[cid].toJson());
    const auto& operations = snapshot["operations"].toArray();
    // Like the desktop's send backlog, at most about two full histories wait
    // for a slow peer before the link is dropped.
    if (link->outboxOperations + operations.size() > 2 * std::size_t(kMaxOperations))
        throw std::runtime_error("community connection is backlogged");
    // Established members stay online while exchanging newer metadata.
    link->synchronizing[cid] = now();
    send(link, {{"type", "state-begin"},
                {"community", cid},
                {"owner", models_[cid].owner},
                {"count", int(operations.size())}});
    for (const auto& operation : operations)
        link->outbox.push_back({{}, cid, operation});
    link->outboxOperations += operations.size();
    send(link, {{"type", "state-end"}, {"community", cid}});
    pump(link);
}

std::vector<Client::Link*> Client::connections() const {
    std::vector<Link*> result;
    for (const auto& entry : peers_)
        if (!entry.second->dead)
            result.push_back(entry.second);
    return result;
}

void Client::sendEach(Link* link, const std::function<void()>& action) {
    // A peer whose send buffer is full is dropped without stopping delivery
    // to the other peers.
    try {
        action();
    }
    catch (const std::exception& error) {
        dropLink(link, error.what());
    }
}

void Client::broadcastState(const std::string& cid) {
    for (auto* link : connections()) {
        if (writable(link, cid)) {
            link->allowed.insert(cid);
            sendEach(link, [&] { sendState(link, cid); });
        }
        else {
            // Deliver the signed removal/tombstone before withdrawing access.
            if (link->allowed.count(cid))
                sendEach(link, [&] { sendState(link, cid); });
            link->allowed.erase(cid);
            link->ready.erase(cid);
            link->synchronizing.erase(cid);
            presence_[cid].erase(link->peer);
        }
    }
}

void Client::handle(Link* link, const json::Value& message) {
    const auto& type = message["type"].toString();
    const auto& cid = message["community"].toString();
    if (type == "hello") {
        if (link->hello || message["protocol"].toString() != "disquisition-community" ||
            message["v"].toInt() != 1 || message["peer"].toString() != link->peer ||
            message["nonce"].toString().size() != 32 || message["communities"].toArray().size() > 64)
            throw std::runtime_error("invalid community handshake");
        link->hello = true;
        link->remoteNonce = message["nonce"].toString();
        for (const auto& value : message["communities"].toArray())
            link->claimed.insert(value["id"].toString());
        offer(link);
        notify(Event::Kind::Changed);
        return;
    }
    if (!link->hello)
        throw std::runtime_error("community handshake required");
    if (type == "ping") {
        send(link, {{"type", "pong"}});
        return;
    }
    if (type == "pong")
        return;
    if (type == "join") {
        const auto& intent = message["intent"];
        if (intent["peer"].toString() != link->peer || intent["nonce"].toString() != link->nonce ||
            intent["host"].toString() != peerId() || intent["community"].toString() != cid ||
            link->admissions.size() >= 16 || json::canonical(intent).size() > 2048)
            throw std::runtime_error("invalid admission request");
        link->admissions[cid] = intent.toObject();
        processAdmissions();
        return;
    }
    if (type == "rejected") {
        const auto joining = joining_.find(cid);
        if (joining != joining_.end() && joining->second == link && pending_.erase(cid)) {
            joining_.erase(cid);
            joinStarted_.erase(cid);
            persistPending();
            Event event;
            event.kind = Event::Kind::JoinRejected;
            event.community = cid;
            event.message = "Invite rejected";
            events_.push_back(std::move(event));
        }
        return;
    }
    if (type == "redirect") {
        if (!pending_.count(cid))
            return;
        const auto& peer = message["peer"].toString();
        const auto& endpoint = message["endpoint"].toString();
        if (!isPeerId(peer) || peer == peerId() || !validEndpoint(endpoint))
            throw std::runtime_error("invalid host redirect");
        auto& seeds = pending_[cid].seeds;
        const json::Value route(json::Object{{"peer", peer}, {"endpoint", endpoint}});
        const auto existing = std::find(seeds.begin(), seeds.end(), route);
        if (existing == seeds.end()) {
            if (seeds.size() >= 8)
                seeds.pop_back();
            seeds.insert(seeds.begin(), route);
            persistPending();
        }
        else {
            seeds.erase(existing);
            seeds.insert(seeds.begin(), route);
        }
        joining_.erase(cid);
        joinStarted_.erase(cid);
        return;
    }
    if (type == "offer") {
        const auto model = models_.find(cid);
        if (!writable(link, cid)) {
            // A returning member may have missed a departure or deletion.
            // Send the signed final state to members of the retained history.
            if (model != models_.end() && !model->second.isMember(peerId()) &&
                model->second.members.count(link->peer)) {
                link->allowed.insert(cid);
                sendState(link, cid);
                link->allowed.erase(cid);
                link->synchronizing.erase(cid);
            }
            else if (model != models_.end() && model->second.isMember(peerId())) {
                // We may have missed this peer's admission while offline.
                // Request its signed history without granting it access to ours.
                send(link, {{"type", "state-request"}, {"community", cid}});
            }
            return;
        }
        link->allowed.insert(cid);
        link->claimed.insert(cid);
        if (message["revision"].toString() == model->second.revision()) {
            link->ready.insert(cid);
            link->synchronizing.erase(cid);
            send(link, {{"type", "synced"}, {"community", cid}, {"revision", model->second.revision()}});
            send(link, presenceObject(cid, mine_[cid]));
        }
        else
            sendState(link, cid);
        notify(Event::Kind::Changed);
        return;
    }
    if (type == "state-request") {
        if (writable(link, cid))
            sendState(link, cid);
        return;
    }
    if (type == "state-begin") {
        if ((!models_.count(cid) && !pending_.count(cid)) || link->snapshots.count(cid) ||
            link->snapshots.size() >= 64)
            throw std::runtime_error("unexpected community snapshot");
        const int count = message["count"].toInt();
        if (count < 1 || count > kMaxOperations)
            throw std::runtime_error("invalid snapshot count");
        const auto owner = models_.count(cid) ? models_[cid].owner : pending_[cid].owner;
        if (owner != message["owner"].toString())
            throw std::runtime_error("community owner mismatch");
        link->synchronizing[cid] = now();
        Link::Snapshot snapshot;
        snapshot.owner = owner;
        snapshot.count = count;
        snapshot.started = now();
        link->snapshots[cid] = std::move(snapshot);
        return;
    }
    if (type == "state-operation") {
        const auto found = link->snapshots.find(cid);
        if (found == link->snapshots.end())
            throw std::runtime_error("snapshot was not started");
        auto& snapshot = found->second;
        snapshot.bytes += json::canonical(message["operation"]).size();
        std::size_t buffered = 0;
        for (const auto& connection : links_)
            for (const auto& partial : connection->snapshots)
                buffered += partial.second.bytes;
        if (snapshot.operations.size() >= std::size_t(snapshot.count) ||
            snapshot.bytes > kMaxMetadataBytes)
            throw std::runtime_error("snapshot exceeds its limits");
        if (buffered > kMaxSnapshotBuffer)
            throw std::runtime_error("too many metadata transfers in progress");
        snapshot.operations.push_back(message["operation"]);
        link->synchronizing[cid] = now();
        return;
    }
    if (type == "state-end") {
        const auto found = link->snapshots.find(cid);
        if (found == link->snapshots.end())
            throw std::runtime_error("snapshot was not started");
        if (found->second.operations.size() != std::size_t(found->second.count))
            throw std::runtime_error("incomplete community snapshot");
        const json::Value object(json::Object{{"v", 1},
                                              {"id", cid},
                                              {"owner", found->second.owner},
                                              {"operations", std::move(found->second.operations)}});
        found->second.complete = true;
        mergeSnapshot(link, cid, object);
        return;
    }
    if (type == "synced") {
        if (!writable(link, cid) || message["revision"].toString() != models_[cid].revision())
            return;
        link->allowed.insert(cid);
        link->ready.insert(cid);
        link->synchronizing.erase(cid);
        send(link, presenceObject(cid, mine_[cid]));
        notify(Event::Kind::Changed);
        return;
    }
    if (!writable(link, cid) || !link->allowed.count(cid) || !link->ready.count(cid))
        return;
    const auto& model = models_.at(cid);
    if (type == "move-voice") {
        const auto& channel = message["channel"].toString();
        const auto target = model.channels.find(channel);
        if (!model.canAdmin(link->peer) || mine_[cid].voiceChannel.empty() ||
            target == model.channels.end() || target->second.type != "voice")
            return;
        const auto previous = mine_[cid];
        setVoice(cid, channel, previous.muted, previous.deafened);
        Event event;
        event.kind = Event::Kind::VoiceMoved;
        event.community = cid;
        event.channel = channel;
        events_.push_back(std::move(event));
        return;
    }
    if (type == "presence") {
        const auto& channel = message["channel"].toString();
        if (!channel.empty()) {
            const auto found = model.channels.find(channel);
            if (found == model.channels.end() || found->second.type != "voice")
                return;
        }
        presence_[cid][link->peer] = {channel, message["muted"].toBool(),
                                      message["deafened"].toBool(),
                                      !channel.empty() && message["sharing"].toBool()};
        notify(Event::Kind::Changed);
        return;
    }
    if (type == "packet") {
        const auto& channel = message["channel"].toString();
        const auto& kind = message["kind"].toString();
        const auto& data = message["data"];
        const auto& id = data["id"].toString();
        const auto key = link->peer + ':' + cid + ':' + id;
        if (id.size() > 64 || id.empty() || seenMessages_.count(key) ||
            !validPacket(cid, channel, kind, data, link->peer))
            return;
        seenMessages_.insert(key);
        messageOrder_.push_back(key);
        if (messageOrder_.size() > 4096) {
            seenMessages_.erase(messageOrder_.front());
            messageOrder_.pop_front();
        }
        Event event;
        event.kind = Event::Kind::Packet;
        event.community = cid;
        event.channel = channel;
        event.peer = link->peer;
        event.type = kind;
        event.data = data;
        events_.push_back(std::move(event));
    }
    // Wake-ups, screens, cameras, drawings and sprites are desktop features.
}

bool Client::synchronizing(const std::string& cid) const {
    if (!running_)
        return false;
    if (now() < recoveringUntil_)
        return true;
    const auto model = models_.find(cid);
    for (const auto& link : links_) {
        if (link->dead || !link->hello || model == models_.end() ||
            !model->second.isMember(link->peer))
            continue;
        const auto started = link->synchronizing.find(cid);
        if (!link->ready.count(cid) || started != link->synchronizing.end()) {
            // A stalled transfer still leaves the link alive, but it must not
            // suspend every other admission.
            if (started != link->synchronizing.end() && now() - started->second > 20000)
                continue;
            return true;
        }
    }
    return false;
}

void Client::processAdmissions() {
    std::vector<Link*> connections;
    for (const auto& entry : peers_)
        connections.push_back(entry.second);
    for (auto* link : connections) {
        if (link->dead)
            continue;
        std::vector<std::string> communities;
        for (const auto& entry : link->admissions)
            communities.push_back(entry.first);
        for (const auto& cid : communities) {
            if (synchronizing(cid))
                continue;
            const auto intent = link->admissions[cid];
            link->admissions.erase(cid);
            try {
                if (!models_.count(cid) || !models_[cid].isMember(peerId()))
                    throw std::runtime_error("join rejected");
                auto model = models_[cid];
                if (!model.acceptsIntent(peerId(), intent))
                    throw std::runtime_error("join rejected");
                const auto preferred = model.host(online(cid));
                if (!preferred.empty() && preferred != peerId()) {
                    // A bootstrap peer validates the invite before revealing a
                    // routing hint, then lets the preferred host admit the peer.
                    send(link, {{"type", "redirect"},
                                {"community", cid},
                                {"peer", preferred},
                                {"endpoint", model.members[preferred].endpoint}});
                    if (link->allowed.empty() && link->admissions.empty()) {
                        link->closing = true;
                        pump(link);
                        break;
                    }
                    continue;
                }
                if (!model.isMember(json::Value(intent)["peer"].toString()))
                    model.admit(*identity_, intent);
                save(std::move(model));
                link->claimed.insert(cid);
                link->allowed.insert(cid);
                broadcastState(cid);
            }
            catch (const std::exception&) {
                send(link, {{"type", "rejected"}, {"community", cid}});
                if (link->allowed.empty() && link->admissions.empty()) {
                    link->closing = true;
                    pump(link);
                    break;
                }
            }
        }
    }
}

std::set<std::string> Client::online(const std::string& cid) const {
    std::set<std::string> result;
    const auto model = models_.find(cid);
    if (!running_ || model == models_.end() || !model->second.isMember(peerId()))
        return result;
    result.insert(peerId());
    for (const auto& [peer, link] : peers_)
        if (!link->dead && writable(link, cid) && link->ready.count(cid))
            result.insert(peer);
    return result;
}

Presence Client::presence(const std::string& cid, const std::string& peer) const {
    if (peer == peerId()) {
        const auto found = mine_.find(cid);
        return found == mine_.end() ? Presence{} : found->second;
    }
    const auto group = presence_.find(cid);
    if (group == presence_.end())
        return {};
    const auto found = group->second.find(peer);
    return found == group->second.end() ? Presence{} : found->second;
}

void Client::setVoice(const std::string& cid, const std::string& channel, bool muted,
                      bool deafened) {
    if (!channel.empty()) {
        const auto model = models_.find(cid);
        if (model == models_.end() || !model->second.isMember(peerId()) ||
            !model->second.channels.count(channel) ||
            model->second.channels.at(channel).type != "voice")
            throw std::runtime_error("unknown voice channel");
    }
    // One voice channel at a time across communities.
    for (auto& [other, presence] : mine_) {
        if (other == cid || presence.voiceChannel.empty())
            continue;
        presence = {};
        for (auto* link : connections())
            if (!link->dead && writable(link, other) && link->ready.count(other))
                sendEach(link, [&] { send(link, presenceObject(other, {})); });
    }
    mine_[cid] = {channel, muted, deafened, false};
    for (auto* link : connections())
        if (!link->dead && writable(link, cid) && link->ready.count(cid))
            sendEach(link, [&] { send(link, presenceObject(cid, mine_[cid])); });
    notify(Event::Kind::Changed);
}

bool Client::validPacket(const std::string& cid, const std::string& channel,
                         const std::string& type, const json::Value& data,
                         const std::string& peer) const {
    const auto model = models_.find(cid);
    if (model == models_.end() || !model->second.isMember(peerId()) ||
        !model->second.isMember(peer))
        return false;
    const auto timestamp = data["timestamp"].toDouble(-1);
    if (timestamp < 1 || timestamp > 9007199254740991.0)
        return false;
    const auto& target = data["target"].toString();
    if (!target.empty() && target != peerId())
        return false;
    const auto found = model->second.channels.find(channel);
    if (found == model->second.channels.end())
        return false;
    if (type == "chat") {
        const auto& body = data["body"].toString();
        return found->second.type == "text" && !json::blank(body) && body.size() <= 2000;
    }
    if (type == "image")
        return found->second.type == "text" && json::canonical(data).size() <= 24 * 1024;
    const auto local = presence(cid, peerId());
    const auto remote = presence(cid, peer);
    if (found->second.type != "voice" || local.voiceChannel != channel ||
        remote.voiceChannel != channel)
        return false;
    if (type == "audio")
        return !local.deafened && !remote.muted && fromBase64(data["bytes"].toString()).size() == 640;
    return false;
}

int Client::sendPacket(const std::string& cid, const std::string& channel, const std::string& type,
                       json::Object data) {
    const auto model = models_.find(cid);
    if (!running_ || model == models_.end() || !model->second.isMember(peerId()) ||
        !model->second.channels.count(channel))
        throw std::runtime_error("unknown channel");
    const auto& channelType = model->second.channels.at(channel).type;
    if ((type == "chat" || type == "image") ? channelType != "text" : channelType != "voice")
        throw std::runtime_error("message does not belong in this channel");
    const json::Value view(data);
    if (type == "chat" &&
        (json::blank(view["body"].toString()) || view["body"].toString().size() > 2000))
        throw std::runtime_error("messages must contain 1 to 2000 bytes");
    if (view["id"].toString().empty())
        data["id"] = freshId();
    if (!view["timestamp"].isNumber())
        data["timestamp"] = json::Value(std::int64_t(clock_.wallMs()));
    const json::Object packet{{"type", "packet"},
                              {"community", cid},
                              {"channel", channel},
                              {"kind", type},
                              {"data", std::move(data)}};
    int recipients = 0;
    const auto local = presence(cid, peerId());
    for (auto* link : connections()) {
        if (link->dead || !writable(link, cid) || !link->ready.count(cid))
            continue;
        if (channelType == "voice" &&
            (local.voiceChannel != channel || presence(cid, link->peer).voiceChannel != channel))
            continue;
        if (type == "audio" && (link->stream.backlog() > kPaceBytes || !link->outbox.empty()))
            continue;
        sendEach(link, [&] {
            send(link, packet);
            ++recipients;
        });
    }
    return recipients;
}

void Client::registerRelay() {
    if (!relay_ || relayNonce_.empty() || relay_->state() != Stream::State::Plain)
        return;
    json::Array communities;
    for (const auto& [id, model] : models_)
        if (model.isMember(peerId()))
            communities.push_back(id);
    relay_->writePlain(frame(json::Value(signedObject({{"type", "register"},
                                                       {"peer", peerId()},
                                                       {"nonce", relayNonce_},
                                                       {"communities", std::move(communities)}},
                                                      *identity_, kRelayDomain))));
}

void Client::pollRelay() {
    if (relayEndpoint_.empty())
        return;
    const auto reset = [this](const std::string& reason) {
        if (!reason.empty())
            relayError_ = reason;
        relay_.reset();
        relayNonce_.clear();
        relayDue_ = now() + 3000;
        if (relayRegistered_) {
            relayRegistered_ = false;
            notify(Event::Kind::Changed);
        }
    };
    std::string scheme, host;
    unsigned port = 0;
    splitEndpoint(relayEndpoint_, scheme, host, port);
    const auto address = authority(relayEndpoint_);
    if (!relay_) {
        if (relayDue_ > now())
            return;
        relayDue_ = now() + 3000;
        relay_ = std::make_unique<Stream>();
        relayLastRead_ = now();
        relayPinged_ = now();
        if (!relay_->connect(host, port, now())) {
            reset("Cannot reach relay " + address + ". " + relay_->error());
            return;
        }
        relay_->writePlain(std::string(kRelayPrefix, kRelayPrefixSize));
    }
    if (!relay_->poll(now())) {
        reset(relayNonce_.empty()
                  ? "Relay " + address +
                        " closed the community handshake. Check the address and that the relay is "
                        "up to date."
                  : "Cannot reach relay " + address + ". " + relay_->error());
        return;
    }
    try {
        json::Value message;
        while (relay_ && takeFrame(relay_->plain(), message, 64 * 1024)) {
            relayLastRead_ = now();
            const auto& type = message["type"].toString();
            if (type == "challenge") {
                relayNonce_ = message["nonce"].toString();
                registerRelay();
            }
            else if (type == "registered") {
                relayError_.clear();
                if (!relayRegistered_) {
                    relayRegistered_ = true;
                    retries_.clear();
                    notify(Event::Kind::Changed);
                }
            }
            else if (type == "error") {
                reset(message["message"].toString());
                return;
            }
            else if (type == "incoming") {
                if (links_.size() >= kMaxLinks)
                    continue;
                auto* link = makeLink(false, message["peer"].toString());
                link->relayAction = "accept";
                link->ticket = message["ticket"].toString();
                if (!link->stream.connect(host, port, now()))
                    dropLink(link);
            }
        }
    }
    catch (const std::exception&) {
        reset("Relay " + address + " sent an invalid response.");
        return;
    }
    if (now() - relayLastRead_ > (relayRegistered_ ? 35000u : 10000u)) {
        reset("Relay " + address + " is not responding.");
        return;
    }
    if (!relayNonce_.empty() && now() - relayPinged_ >= 10000) {
        relayPinged_ = now();
        relay_->writePlain(frame(json::Value(json::Object{{"type", "ping"}})));
    }
}

void Client::tick() {
    if (!running_)
        return;
    try {
        if (recoveringUntil_ != 0 && now() >= recoveringUntil_) {
            recoveringUntil_ = 0;
            notify(Event::Kind::Changed);
        }
        for (const auto& owned : links_) {
            auto* link = owned.get();
            if (link->dead)
                continue;
            bool stalled = false;
            for (const auto& snapshot : link->snapshots)
                if (!snapshot.second.complete && now() - snapshot.second.started > 20000)
                    stalled = true;
            if ((!link->hello && now() - link->started > 10000) ||
                (link->hello && now() - link->touched > 35000) || stalled) {
                dropLink(link);
                continue;
            }
            if (link->hello && link->allowed.empty() && link->admissions.empty() &&
                now() - link->started > 30000) {
                dropLink(link);
                continue;
            }
            if (link->hello && now() - link->touched > 10000)
                send(link, {{"type", "ping"}});
        }
        for (const auto& [cid, model] : models_) {
            if (!model.isMember(peerId()))
                continue;
            for (const auto& [peer, member] : model.members) {
                if (peer == peerId() || peers_.count(peer))
                    continue;
                // Either side may dial after a restart; duplicate selection is
                // deterministic and the registry still retains one connection.
                dial(peer, member.endpoint);
            }
        }
        for (auto& [cid, invite] : pending_) {
            if (joining_.count(cid)) {
                if (now() - joinStarted_[cid] <= 20000)
                    continue;
                joining_.erase(cid);
                joinStarted_.erase(cid);
                if (invite.seeds.size() > 1) {
                    auto seed = invite.seeds.front();
                    invite.seeds.erase(invite.seeds.begin());
                    invite.seeds.push_back(std::move(seed));
                }
            }
            for (const auto& seed : invite.seeds) {
                const auto& peer = seed["peer"].toString();
                const auto& endpoint = seed["endpoint"].toString();
                Link* selected = nullptr;
                const auto connected = peers_.find(peer);
                if (connected != peers_.end() && !connected->second->dead)
                    selected = connected->second;
                if (!selected) {
                    dial(peer, endpoint, cid);
                    const auto route = peer + ':' + endpoint + ':' + cid;
                    for (const auto& connection : links_)
                        if (!connection->dead && connection->route == route) {
                            selected = connection.get();
                            break;
                        }
                }
                if (selected) {
                    joining_[cid] = selected;
                    joinStarted_[cid] = now();
                    if (selected->hello)
                        offer(selected);
                    break;
                }
            }
        }
        processAdmissions();
    }
    catch (const std::exception& error) {
        notify(Event::Kind::Error, {}, error.what());
    }
}

void Client::poll() {
    if (!running_)
        return;
    pollRelay();
    for (std::size_t index = 0; index < links_.size(); ++index) {
        auto* link = links_[index].get();
        if (!link->dead)
            pollLink(link);
    }
    if (now() - lastTick_ >= 500) {
        lastTick_ = now();
        tick();
    }
    links_.erase(std::remove_if(links_.begin(), links_.end(),
                                [](const std::unique_ptr<Link>& link) { return link->dead; }),
                 links_.end());
}

} // namespace console
