#include "relay/relay.h"

#include <unistd.h>

#include <algorithm>
#include <iterator>
#include <utility>
#include <vector>

#include "common/net.h"
#include "util/log.h"

namespace chat {
namespace {

using Clock = std::chrono::steady_clock;

constexpr int kServerConnectTimeoutMs = 3000;
constexpr auto kServerRetryInterval = std::chrono::seconds(3);
constexpr auto kTickInterval = std::chrono::milliseconds(50);

void eraseName(std::vector<std::string>& names, const std::string& name) {
    names.erase(std::remove(names.begin(), names.end(), name), names.end());
}

} // namespace

Relay::Relay(RelayOptions options) : options_(std::move(options)) {
    options_.advertiseHost = trim(options_.advertiseHost);
}

Relay::~Relay() {
    shutdown();
}

int Relay::run() {
    logToConsole("relay");
    ignoreSigpipe();
    installShutdownHandlers();

    std::string error;
    if (!listen(error)) {
        LOG_ERR("cannot listen on port {}: {}", options_.port, error);
        return 1;
    }
    running_.store(true);
    acceptThread_ = std::thread([this] { acceptLoop(); });
    if (options_.advertiseHost.empty()) {
        LOG_INFO("listening on port {}", boundPort_);
        LOG_WARN("no --advertise host: clients that cannot accept connections will not dial "
                 "users hosted here");
    }
    else {
        LOG_INFO("listening on port {}, advertising {}", boundPort_, options_.advertiseHost);
    }

    // Keep one anonymous server connection for health probes. It never joins
    // the room, so checking the relay cannot churn a user's session.
    healthServer_.connectTo(options_.serverHost, options_.serverPort, 2000, error);
    if (!healthServer_.failed()) healthServer_.startReader();
    nextHealthAttempt_ = Clock::now() + kServerRetryInterval;

    while (!shutdownRequested()) {
        serviceHealth();
        dispatchInbound();
        serviceUsers();
        std::this_thread::sleep_for(kTickInterval);
    }

    LOG_INFO("shutting down");
    shutdown();
    return 0;
}

bool Relay::listen(std::string& error) {
    listenFd_ = listenTcp(options_.port, error);
    if (listenFd_ < 0) {
        return false;
    }
    boundPort_ = localPort(listenFd_);
    if (boundPort_ == 0) {
        boundPort_ = options_.port;
    }
    return true;
}

void Relay::shutdown() {
    running_.store(false);
    if (acceptThread_.joinable()) {
        acceptThread_.join();
    }
    if (listenFd_ >= 0) {
        ::close(listenFd_);
        listenFd_ = -1;
    }
    users_.clear();
    awaiting_.clear();
    healthServer_.stop();
}

void Relay::acceptLoop() {
    acceptConnections(listenFd_, running_, [this](int fd) {
        if (auto connection = Connection::fromAccepted(fd)) {
            std::lock_guard<std::mutex> lock(mutex_);
            awaiting_.push_back(std::move(connection));
        }
    });
}

// ---------------------------------------------------------------------------
// Inbound connections

void Relay::dispatchInbound() {
    std::deque<std::unique_ptr<Connection>> pending;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        pending.swap(awaiting_);
    }

    std::deque<std::unique_ptr<Connection>> undecided;
    for (std::unique_ptr<Connection>& connection : pending) {
        Message first;
        if (connection->poll(first)) {
            route(std::move(connection), first);
        }
        else if (!connection->failed()) {
            undecided.push_back(std::move(connection));
        }
    }

    // Put the undecided ones back ahead of anything accepted meanwhile.
    std::lock_guard<std::mutex> lock(mutex_);
    awaiting_.insert(awaiting_.begin(), std::make_move_iterator(undecided.begin()),
                     std::make_move_iterator(undecided.end()));
}

// Anything that is not a valid Login or Hello is closed.
void Relay::route(std::unique_ptr<Connection> connection, const Message& first) {
    if (first.type == MsgType::Login) {
        const std::string requested = first.fields.empty() ? "" : sanitizeName(first.fields[0]);
        if (requested.empty()) {
            return;
        }
        if (users_.size() >= options_.maxUsers && users_.count(requested) == 0) {
            LOG_WARN("rejected {}: relay is full", requested);
            connection->send(Message{MsgType::Error, {"relay is full"}});
            return;
        }
        attachClient(requested, std::move(connection));
    }
    else if (first.type == MsgType::Hello && first.fields.size() >= 2) {
        const std::string sender = sanitizeName(first.fields[0]);
        User* target = findUser(sanitizeName(first.fields[1]));
        if (target != nullptr && !sender.empty()) {
            target->peers.adoptInbound(sender, std::move(connection));
        }
    }
    else if (first.type == MsgType::RelayProbe && first.fields.empty() &&
             !healthServer_.failed()) {
        connection->send(Message{MsgType::RelayReady, {}});
    }
}

Relay::User* Relay::findUser(const std::string& name) {
    for (auto& [key, user] : users_) {
        if (user->assignedName == name ||
            (user->assignedName.empty() && user->requestedName == name)) {
            return user.get();
        }
    }
    return nullptr;
}

void Relay::attachClient(const std::string& requestedName, std::unique_ptr<Connection> client) {
    const auto existing = users_.find(requestedName);
    if (existing != users_.end()) {
        User& user = *existing->second;
        if (user.client != nullptr) {
            user.client->stop();
        }
        user.client = std::move(client);
        user.clientWaiting = true;
        LOG_INFO("{} client reattached", requestedName);
        if (user.serverReady) {
            serveClientLogin(user);
        }
        return;
    }

    auto user = std::make_unique<User>();
    user->requestedName = requestedName;
    user->client = std::move(client);
    user->nextServerAttempt = Clock::now();
    user->peers.setMyName(requestedName);
    user->peers.setMyAdvertised(!options_.advertiseHost.empty());
    std::string error;
    if (!user->peers.startDetached(error)) {
        LOG_ERR("cannot start mesh for {}: {}", requestedName, error);
        return;
    }

    User& added = *users_.emplace(requestedName, std::move(user)).first->second;
    LOG_INFO("{} client attached", requestedName);
    connectServer(added);
}

void Relay::dropUser(const std::string& key) {
    const auto departing = users_.find(key);
    if (departing == users_.end()) {
        return;
    }
    const std::string requestedName = departing->second->requestedName;
    const std::string assignedName = departing->second->assignedName.empty()
                                         ? requestedName
                                         : departing->second->assignedName;
    users_.erase(departing);

    // The server announces departures, but while it is unreachable only the
    // relay knows, so update the other hosted users directly.
    for (auto& [otherKey, user] : users_) {
        for (const std::string& name : {requestedName, assignedName}) {
            user->roster.erase(name);
            user->peers.removePeer(name);
            eraseName(user->lastUsers.fields, name);
        }
        if (!user->serverReady) {
            sendToClient(*user, Message{MsgType::PeerLeft, {assignedName}});
            sendToClient(*user, user->lastUsers);
        }
    }
}

// ---------------------------------------------------------------------------
// Hosted users

void Relay::serviceUsers() {
    std::vector<std::string> departed;
    for (auto& [key, user] : users_) {
        if (!serviceUser(*user)) {
            LOG_INFO("{} client disconnected", user->requestedName);
            departed.push_back(key);
        }
    }
    for (const std::string& key : departed) {
        dropUser(key);
    }
}

void Relay::serviceHealth() {
    if (!healthServer_.failed() || Clock::now() < nextHealthAttempt_) return;
    healthServer_.stop();
    std::string error;
    if (healthServer_.connectTo(options_.serverHost, options_.serverPort, 2000, error)) {
        healthServer_.startReader();
    }
    nextHealthAttempt_ = Clock::now() + kServerRetryInterval;
}

// Returns false once the user's client has disconnected.
bool Relay::serviceUser(User& user) {
    if (user.server.failed()) {
        user.serverReady = false;
        if (Clock::now() >= user.nextServerAttempt) {
            connectServer(user);
            user.nextServerAttempt = Clock::now() + kServerRetryInterval;
        }
    }

    PeerNetwork::Event event;
    while (user.peers.poll(event)) {
        handlePeerEvent(user, event);
    }
    Message message;
    while (user.server.poll(message)) {
        handleServerMessage(user, message);
    }
    if (user.client == nullptr) {
        return true;
    }
    while (user.client->poll(message)) {
        handleClientMessage(user, message);
    }
    return !user.client->failed();
}

void Relay::connectServer(User& user) {
    if (user.server.failed()) {
        // Frames from the dead connection must not confuse the new login.
        Message stale;
        while (user.server.poll(stale)) {
        }
        user.server.stop();
        user.serverReady = false;
    }

    std::string error;
    if (!user.server.connectTo(options_.serverHost, options_.serverPort, kServerConnectTimeoutMs,
                               error)) {
        LOG_DEBUG("{}: cannot reach server: {}", user.requestedName, error);
        return;
    }
    user.server.startReader();
    user.serverReady = false;
    user.server.send(Message{
        MsgType::Login, {user.requestedName, std::to_string(boundPort_), options_.advertiseHost}});
    if (user.client != nullptr) {
        user.clientWaiting = true;
    }
}

void Relay::serveClientLogin(User& user) {
    if (user.client == nullptr || !user.clientWaiting) {
        return;
    }
    user.clientWaiting = false;
    sendToClient(user,
                 Message{MsgType::LoginOk, {user.assignedName, "welcome, " + user.assignedName}});
    replayRoster(user);
}

void Relay::replayRoster(User& user) {
    for (const auto& [name, peer] : user.roster) {
        sendToClient(user, peerMessage(MsgType::Peer, peer));
    }
    if (!user.lastUsers.fields.empty()) {
        sendToClient(user, user.lastUsers);
        return;
    }
    Message users{MsgType::Users, {}};
    if (!user.assignedName.empty()) {
        users.fields.push_back(user.assignedName);
    }
    for (const auto& [name, peer] : user.roster) {
        users.fields.push_back(name);
    }
    sendToClient(user, users);
}

void Relay::handleServerMessage(User& user, const Message& message) {
    switch (message.type) {
        case MsgType::LoginOk:
            if (!message.fields.empty()) {
                user.assignedName = message.fields[0];
            }
            user.peers.setMyName(user.assignedName);
            user.serverReady = true;
            LOG_INFO("{} signed in as '{}'", user.requestedName, user.assignedName);
            serveClientLogin(user);
            break;

        case MsgType::Peer:
        case MsgType::PeerJoined: {
            PeerAddress peer;
            if (parsePeerAddress(message, peer)) {
                user.peers.addPeer(peer.name, peer.host, peer.port, peer.advertised);
                user.roster[peer.name] = std::move(peer);
                sendToClient(user, message);
            }
            break;
        }

        case MsgType::PeerLeft:
            if (!message.fields.empty()) {
                user.roster.erase(message.fields[0]);
                user.peers.removePeer(message.fields[0]);
                sendToClient(user, message);
            }
            break;

        case MsgType::Users:
            user.lastUsers = message;
            sendToClient(user, message);
            break;

        case MsgType::History:
        case MsgType::HistoryEnd:
        case MsgType::System:
        case MsgType::VoiceAudio:
        case MsgType::VoiceState:
            sendToClient(user, message);
            break;

        case MsgType::VoicePort:
            if (message.fields.size() == 2) {
                auto peer = user.roster.find(message.fields[0]);
                std::uint16_t port = 0;
                if (peer != user.roster.end() && parsePort(message.fields[1], port, true)) {
                    peer->second.voicePort = port;
                }
            }
            sendToClient(user, message);
            break;

        case MsgType::Error:
            LOG_WARN("{} server error: {}", user.requestedName,
                     message.fields.empty() ? "unknown" : message.fields[0]);
            sendToClient(user, message);
            break;

        default:
            break;
    }
}

void Relay::handleClientMessage(User& user, const Message& message) {
    switch (message.type) {
        case MsgType::Login:
            if (user.serverReady) {
                serveClientLogin(user);
            }
            break;

        case MsgType::Store:
        case MsgType::FetchHistory:
        case MsgType::VoicePort:
        case MsgType::VoiceAudio:
        case MsgType::VoiceState:
            if (user.serverReady && !user.server.failed()) {
                user.server.send(message);
            }
            break;

        case MsgType::PeerChat: {
            // The mesh stamps the user's own name on outgoing chat.
            ChatPayload payload;
            if (parsePeerChat(message, payload)) {
                user.peers.sendChat(payload.timestamp, payload.body, payload.color);
            }
            break;
        }

        default:
            break;
    }
}

void Relay::handlePeerEvent(User& user, const PeerNetwork::Event& event) {
    switch (event.kind) {
        case PeerNetwork::Event::Kind::Chat:
            sendToClient(user, Message{MsgType::PeerChat,
                                       {event.name, std::to_string(event.timestamp), event.body,
                                        event.color}});
            break;
        case PeerNetwork::Event::Kind::Join:
            LOG_INFO("{}: mesh link up with {}", user.requestedName, event.name);
            break;
        case PeerNetwork::Event::Kind::Leave:
            LOG_INFO("{}: mesh link down with {}", user.requestedName, event.name);
            break;
        case PeerNetwork::Event::Kind::Note:
            LOG_WARN("{}: {}", user.requestedName, event.body);
            break;
    }
}

void Relay::sendToClient(User& user, const Message& message) {
    if (user.client != nullptr) {
        user.client->send(message);
    }
}

} // namespace chat
