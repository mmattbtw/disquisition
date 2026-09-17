#include "relay/relay.h"

#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <csignal>
#include <cstdio>
#include <cstring>
#include <thread>
#include <utility>

namespace chat {
namespace {

volatile std::sig_atomic_t gStopRequested = 0;

void handleStopSignal(int) {
    gStopRequested = 1;
}

void installSignalHandlers() {
    struct sigaction action {};
    action.sa_handler = handleStopSignal;
    sigemptyset(&action.sa_mask);
    sigaction(SIGINT, &action, nullptr);
    sigaction(SIGTERM, &action, nullptr);

    // A peer vanishing mid-write must not take the relay down.
    std::signal(SIGPIPE, SIG_IGN);
}

std::string trimWhitespace(const std::string& text) {
    const auto begin = text.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos) {
        return "";
    }
    const auto end = text.find_last_not_of(" \t\r\n");
    return text.substr(begin, end - begin + 1);
}

}  // namespace

Relay::Relay(RelayOptions options) : options_(std::move(options)) {
    options_.advertiseHost = trimWhitespace(options_.advertiseHost);
}

Relay::~Relay() {
    running_.store(false);
    if (acceptThread_.joinable()) {
        acceptThread_.join();
    }
    if (listenFd_ >= 0) {
        ::close(listenFd_);
        listenFd_ = -1;
    }
    users_.clear();
}

bool Relay::listen(std::string& error) {
    addrinfo hints {};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;

    const std::string service = std::to_string(options_.port);
    addrinfo* results = nullptr;
    const int rc = getaddrinfo(nullptr, service.c_str(), &hints, &results);
    if (rc != 0) {
        error = gai_strerror(rc);
        return false;
    }

    for (addrinfo* entry = results; entry != nullptr; entry = entry->ai_next) {
        const int fd = socket(entry->ai_family, entry->ai_socktype, entry->ai_protocol);
        if (fd < 0) {
            error = std::strerror(errno);
            continue;
        }
        const int enable = 1;
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(enable));
        if (bind(fd, entry->ai_addr, entry->ai_addrlen) == 0 && ::listen(fd, SOMAXCONN) == 0) {
            listenFd_ = fd;
            error.clear();
            break;
        }
        error = std::strerror(errno);
        ::close(fd);
    }
    freeaddrinfo(results);

    if (listenFd_ < 0) {
        if (error.empty()) {
            error = "cannot bind port " + service;
        }
        return false;
    }

    sockaddr_storage address {};
    socklen_t length = sizeof(address);
    if (getsockname(listenFd_, reinterpret_cast<sockaddr*>(&address), &length) == 0) {
        if (address.ss_family == AF_INET6) {
            boundPort_ = ntohs(reinterpret_cast<sockaddr_in6*>(&address)->sin6_port);
        } else if (address.ss_family == AF_INET) {
            boundPort_ = ntohs(reinterpret_cast<sockaddr_in*>(&address)->sin_port);
        }
    }
    if (boundPort_ == 0) {
        boundPort_ = options_.port;
    }
    return true;
}

void Relay::acceptLoop() {
    while (running_.load()) {
        pollfd watched {listenFd_, POLLIN, 0};
        const int ready = ::poll(&watched, 1, 200);
        if (ready <= 0) {
            continue;
        }
        if ((watched.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
            return;
        }
        if ((watched.revents & POLLIN) == 0) {
            continue;
        }

        sockaddr_storage address {};
        socklen_t length = sizeof(address);
        const int fd = accept(listenFd_, reinterpret_cast<sockaddr*>(&address), &length);
        if (fd < 0) {
            continue;
        }

        auto connection = std::make_unique<Connection>();
        if (!connection->adopt(fd)) {
            ::close(fd);
            continue;
        }
        connection->startReader();

        std::lock_guard<std::mutex> lock(mutex_);
        awaiting_.push_back(std::move(connection));
    }
}

Relay::User* Relay::findUser(const std::string& name) {
    for (auto& entry : users_) {
        User& user = *entry.second;
        if (user.assignedName == name ||
            (user.assignedName.empty() && user.requestedName == name)) {
            return &user;
        }
    }
    return nullptr;
}

void Relay::dispatchInbound() {
    std::deque<std::unique_ptr<Connection>> pending;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        pending.swap(awaiting_);
    }

    for (std::size_t index = pending.size(); index-- > 0;) {
        Connection& connection = *pending[index];
        Message first;
        bool handled = false;

        if (connection.poll(first)) {
            handled = true;
            if (first.type == MsgType::Login) {
                const std::string requested =
                    first.fields.empty() ? "" : sanitizeName(first.fields[0]);
                if (!requested.empty()) {
                    if (users_.size() >= options_.maxUsers && users_.find(requested) == users_.end()) {
                        connection.send(Message {MsgType::Error, {"relay is full"}});
                    } else {
                        attachClient(requested, std::move(pending[index]));
                    }
                }
            } else if (first.type == MsgType::Hello && first.fields.size() >= 2) {
                // A peer or another relay dialling one of our users: the Hello
                // names both the caller and the user it is looking for.
                const std::string sender = sanitizeName(first.fields[0]);
                const std::string target = sanitizeName(first.fields[1]);
                User* user = findUser(target);
                if (user != nullptr && !sender.empty()) {
                    user->peers.adoptInbound(sender, std::move(pending[index]));
                }
            }
        } else if (connection.failed()) {
            handled = true;
        }

        if (handled) {
            pending.erase(pending.begin() + static_cast<std::ptrdiff_t>(index));
        }
    }

    if (!pending.empty()) {
        std::lock_guard<std::mutex> lock(mutex_);
        // Older, partially received connections stay ahead of newly accepted
        // ones without sharing the deque while it is being inspected.
        while (!pending.empty()) {
            awaiting_.push_front(std::move(pending.back()));
            pending.pop_back();
        }
    }
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
        log(requestedName + " client reattached");
        if (user.serverReady) {
            serveClientLogin(user);
        }
        return;
    }

    auto user = std::make_unique<User>();
    user->requestedName = requestedName;
    user->client = std::move(client);
    user->nextServerAttempt = std::chrono::steady_clock::now();
    user->peers.setMyName(requestedName);
    user->peers.setMyAdvertised(!options_.advertiseHost.empty());
    std::string error;
    if (!user->peers.startDetached(error)) {
        log("cannot start mesh for " + requestedName + ": " + error);
        return;
    }

    User* raw = user.get();
    users_.emplace(requestedName, std::move(user));
    log(requestedName + " client attached");
    ensureServer(*raw);
}

void Relay::dropUser(const std::string& key) {
    users_.erase(key);
}

bool Relay::ensureServer(User& user) {
    if (user.server.failed()) {
        // Discard anything the dead transport left behind so a re-login cannot
        // be confused by frames from the previous connection.
        Message stale;
        while (user.server.poll(stale)) {
        }
        user.server.stop();
        user.serverReady = false;
    }

    std::string error;
    if (!user.server.connectTo(options_.serverHost, options_.serverPort, 3000, error)) {
        return false;
    }
    user.server.startReader();
    user.serverReady = false;
    user.server.send(Message {MsgType::Login,
                              {user.requestedName, std::to_string(boundPort_),
                               options_.advertiseHost}});
    if (user.client != nullptr) {
        user.clientWaiting = true;
    }
    return true;
}

void Relay::serveClientLogin(User& user) {
    if (user.client == nullptr || !user.clientWaiting) {
        return;
    }
    user.clientWaiting = false;
    sendToClient(user, Message {MsgType::LoginOk, {user.assignedName, "welcome, " + user.assignedName}});
    replayRoster(user);
}

void Relay::replayRoster(User& user) {
    for (const auto& entry : user.roster) {
        sendToClient(user, Message {MsgType::Peer,
                                    {entry.first, entry.second.host, std::to_string(entry.second.port),
                                     entry.second.advertised ? "1" : "0"}});
    }
    Message users {MsgType::Users, {}};
    if (!user.lastUsers.fields.empty()) {
        users = user.lastUsers;
    } else {
        if (!user.assignedName.empty()) {
            users.fields.push_back(user.assignedName);
        }
        for (const auto& entry : user.roster) {
            users.fields.push_back(entry.first);
        }
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
            log(user.requestedName + " signed in as '" + user.assignedName + "'");
            serveClientLogin(user);
            break;

        case MsgType::Peer:
        case MsgType::PeerJoined: {
            if (message.fields.size() < 4) {
                break;
            }
            std::int64_t peerPort = 0;
            if (!parseInt64(message.fields[2], peerPort) || peerPort < 1 || peerPort > 65535) {
                break;
            }
            PeerInfo info;
            info.host = message.fields[1];
            info.port = static_cast<std::uint16_t>(peerPort);
            info.advertised = message.fields[3] == "1";
            user.roster[message.fields[0]] = info;
            user.peers.addPeer(message.fields[0], info.host, info.port, info.advertised);
            sendToClient(user, message);
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
            sendToClient(user, message);
            break;

        case MsgType::Error:
            log(user.requestedName + " server error: " +
                (message.fields.empty() ? std::string("unknown") : message.fields[0]));
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
            if (user.serverReady && !user.server.failed()) {
                user.server.send(message);
            }
            break;

        case MsgType::PeerChat:
            if (message.fields.size() >= 4) {
                std::int64_t timestamp = 0;
                const std::string body = sanitizeBody(message.fields[2]);
                const std::string colour = sanitizeBody(message.fields[3]);
                if (parseInt64(message.fields[1], timestamp) && timestamp > 0 && !body.empty() &&
                    isValidColor(colour)) {
                    user.peers.sendChat(timestamp, body, colour);
                }
            }
            break;

        default:
            break;
    }
}

void Relay::handlePeerEvent(User& user, const PeerNetwork::Event& event) {
    switch (event.kind) {
        case PeerNetwork::Event::Kind::Chat:
            // Re-tag with the name we learned from that peer's own connection:
            // a direct link identifies its sender far more reliably than the
            // frame can.
            sendToClient(user, Message {MsgType::PeerChat,
                                        {event.name, std::to_string(event.timestamp), event.body,
                                         event.colour}});
            break;
        case PeerNetwork::Event::Kind::Join:
            log(user.requestedName + ": mesh link up with " + event.name);
            break;
        case PeerNetwork::Event::Kind::Leave:
            log(user.requestedName + ": mesh link down with " + event.name);
            break;
        case PeerNetwork::Event::Kind::Note:
            log(user.requestedName + ": " + event.body);
            break;
    }
}

void Relay::sendToClient(User& user, const Message& message) {
    if (user.client != nullptr) {
        user.client->send(message);
    }
}

void Relay::serviceUsers() {
    std::vector<std::string> dead;
    for (auto& entry : users_) {
        User& user = *entry.second;

        if (user.server.failed()) {
            user.serverReady = false;
            if (std::chrono::steady_clock::now() >= user.nextServerAttempt) {
                ensureServer(user);
                user.nextServerAttempt =
                    std::chrono::steady_clock::now() + std::chrono::seconds(3);
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

        if (user.client != nullptr) {
            Message incoming;
            while (user.client->poll(incoming)) {
                handleClientMessage(user, incoming);
            }
            if (user.client->failed()) {
                log(user.requestedName + " client disconnected");
                dead.push_back(entry.first);
            }
        }
    }

    for (const std::string& key : dead) {
        dropUser(key);
    }
}

void Relay::log(const std::string& text) const {
    std::printf("[relay] %s\n", text.c_str());
    std::fflush(stdout);
}

int Relay::run() {
    installSignalHandlers();

    std::string error;
    if (!listen(error)) {
        std::fprintf(stderr, "[relay] cannot listen on port %u: %s\n", options_.port, error.c_str());
        return 1;
    }

    running_.store(true);
    acceptThread_ = std::thread([this] { acceptLoop(); });

    log("listening on port " + std::to_string(boundPort_) +
        (options_.advertiseHost.empty() ? "" : ", advertising " + options_.advertiseHost));

    while (gStopRequested == 0) {
        dispatchInbound();
        serviceUsers();
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    log("shutting down");
    running_.store(false);
    if (acceptThread_.joinable()) {
        acceptThread_.join();
    }
    if (listenFd_ >= 0) {
        ::close(listenFd_);
        listenFd_ = -1;
    }
    users_.clear();
    return 0;
}

}  // namespace chat
