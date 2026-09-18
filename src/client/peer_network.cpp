#include "client/peer_network.h"

#include <netdb.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstring>
#include <thread>

#include "common/protocol.h"

namespace chat {
namespace {

constexpr std::size_t kMaxPendingInbound = 16;
constexpr std::size_t kMaxQueuedEvents = 512;
constexpr int kDialAttempts = 3;
constexpr int kDialTimeoutMs = 3000;

bool waitForHelloOk(Connection& connection, const std::string& target) {
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(kDialTimeoutMs);
    while (!connection.failed() && std::chrono::steady_clock::now() < deadline) {
        Message response;
        if (connection.poll(response)) {
            return response.type == MsgType::HelloOk && !response.fields.empty() &&
                   sanitizeName(response.fields[0]) == target;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return false;
}

}  // namespace

PeerNetwork::~PeerNetwork() {
    stop();
}

bool PeerNetwork::start(std::uint16_t preferredPort, std::string& error) {
    if (listenFd_ >= 0) {
        error = "peer listener already started";
        return false;
    }

    addrinfo hints {};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;

    const std::string service = std::to_string(preferredPort);
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
            error = "cannot bind peer port " + service;
        }
        return false;
    }

    sockaddr_storage address {};
    socklen_t length = sizeof(address);
    if (getsockname(listenFd_, reinterpret_cast<sockaddr*>(&address), &length) == 0) {
        if (address.ss_family == AF_INET6) {
            listenPort_ = ntohs(reinterpret_cast<sockaddr_in6*>(&address)->sin6_port);
        } else if (address.ss_family == AF_INET) {
            listenPort_ = ntohs(reinterpret_cast<sockaddr_in*>(&address)->sin_port);
        }
    }
    if (listenPort_ == 0) {
        listenPort_ = preferredPort;
    }

    running_.store(true);
    acceptThread_ = std::thread([this] { acceptLoop(); });
    dialThread_ = std::thread([this] { dialLoop(); });
    return true;
}

void PeerNetwork::stop() {
    if (!running_.exchange(false)) {
        return;
    }
    dialCv_.notify_all();
    if (acceptThread_.joinable()) {
        acceptThread_.join();
    }
    if (dialThread_.joinable()) {
        dialThread_.join();
    }
    if (listenFd_ >= 0) {
        ::close(listenFd_);
        listenFd_ = -1;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    peers_.clear();
    pending_.clear();
    dialQueue_.clear();
    events_.clear();
    listenPort_ = 0;
}

void PeerNetwork::setMyName(const std::string& name) {
    std::lock_guard<std::mutex> lock(mutex_);
    myName_ = name;
    scanPeersForDialsLocked();
}

void PeerNetwork::setMyAdvertised(bool advertised) {
    std::lock_guard<std::mutex> lock(mutex_);
    myAdvertised_ = advertised;
    scanPeersForDialsLocked();
}

void PeerNetwork::setPassive(bool passive) {
    std::lock_guard<std::mutex> lock(mutex_);
    passive_ = passive;
}

bool PeerNetwork::startDetached(std::string& error) {
    if (listenFd_ >= 0 || running_.load()) {
        error = "peer network already started";
        return false;
    }
    error.clear();
    running_.store(true);
    dialThread_ = std::thread([this] { dialLoop(); });
    return true;
}

void PeerNetwork::adoptInbound(const std::string& name, std::unique_ptr<Connection> connection) {
    const std::string cleaned = sanitizeName(name);
    if (cleaned.empty() || connection == nullptr) {
        return;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    if (cleaned == myName_) {
        return;
    }
    auto existing = peers_.find(cleaned);
    if (existing != peers_.end()) {
        if (existing->second.connection != nullptr) {
            return;
        }
        if (!connection->send(Message {MsgType::HelloOk, {myName_}})) {
            return;
        }
        existing->second.connection = std::move(connection);
        existing->second.dialing = false;
    } else {
        if (!connection->send(Message {MsgType::HelloOk, {myName_}})) {
            return;
        }
        Peer peer;
        peer.name = cleaned;
        peer.connection = std::move(connection);
        peer.dialing = false;
        peers_.emplace(cleaned, std::move(peer));
    }
    pushEvent(Event::Kind::Join, cleaned, "", 0);
}

void PeerNetwork::addPeer(const std::string& name, const std::string& host, std::uint16_t port,
                          bool advertised) {
    const std::string cleaned = sanitizeName(name);
    if (cleaned.empty() || port == 0) {
        return;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    if (cleaned == myName_) {
        return;
    }
    auto existing = peers_.find(cleaned);
    if (existing != peers_.end()) {
        Peer& peer = existing->second;
        peer.host = host;
        peer.port = port;
        peer.advertised = advertised;
        if (peer.connection == nullptr && !peer.dialing && shouldDial(peer)) {
            peer.dialing = true;
            enqueueDial(peer.name);
        }
        return;
    }
    Peer peer;
    peer.name = cleaned;
    peer.host = host;
    peer.port = port;
    peer.advertised = advertised;
    peer.dialing = shouldDial(peer);
    const bool dial = peer.dialing;
    peers_.emplace(cleaned, std::move(peer));
    if (dial) {
        enqueueDial(cleaned);
    }
}

void PeerNetwork::removePeer(const std::string& name) {
    std::lock_guard<std::mutex> lock(mutex_);
    peers_.erase(name);
}

bool PeerNetwork::connectedTo(const std::string& name) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = peers_.find(name);
    return it != peers_.end() && it->second.connection != nullptr;
}

bool PeerNetwork::peerAddress(const std::string& name, std::string& host, std::uint16_t& port) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = peers_.find(name);
    if (it == peers_.end()) {
        return false;
    }
    host = it->second.host;
    port = it->second.port;
    return true;
}

std::size_t PeerNetwork::connectedCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::size_t count = 0;
    for (const auto& entry : peers_) {
        if (entry.second.connection != nullptr) {
            ++count;
        }
    }
    return count;
}

bool PeerNetwork::settled() const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!dialQueue_.empty() || !pending_.empty()) {
        return false;
    }
    for (const auto& entry : peers_) {
        if (entry.second.dialing) {
            return false;
        }
    }
    return true;
}

void PeerNetwork::sendChat(std::int64_t timestamp, const std::string& body,
                           const std::string& color) {
    const Message message {MsgType::PeerChat, {myName_, std::to_string(timestamp), body, color}};
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& entry : peers_) {
        if (entry.second.connection != nullptr) {
            entry.second.connection->send(message);
        }
    }
}

bool PeerNetwork::poll(Event& out) {
    std::lock_guard<std::mutex> lock(mutex_);
    drainPending();
    drainPeers();
    if (events_.empty()) {
        return false;
    }
    out = std::move(events_.front());
    events_.pop_front();
    return true;
}

void PeerNetwork::acceptLoop() {
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
        if (pending_.size() >= kMaxPendingInbound) {
            pending_.erase(pending_.begin());
        }
        pending_.push_back(std::move(connection));
    }
}

void PeerNetwork::dialLoop() {
    for (;;) {
        std::string target;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            dialCv_.wait(lock, [this] { return !running_.load() || !dialQueue_.empty(); });
            if (!running_.load()) {
                return;
            }
            if (dialQueue_.empty()) {
                continue;
            }
            target = dialQueue_.front();
            dialQueue_.pop_front();

            const auto it = peers_.find(target);
            if (it == peers_.end() || it->second.connection != nullptr || !it->second.dialing) {
                continue;
            }
        }

        std::string host;
        std::uint16_t port = 0;
        std::string announced;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            const auto it = peers_.find(target);
            if (it == peers_.end()) {
                continue;
            }
            host = it->second.host;
            port = it->second.port;
            announced = myName_;
        }

        bool done = false;
        for (int attempt = 0; attempt < kDialAttempts && running_.load() && !done; ++attempt) {
            auto candidate = std::make_unique<Connection>();
            std::string error;
            if (candidate->connectTo(host, port, kDialTimeoutMs, error)) {
                candidate->startReader();
                if (candidate->send(Message {MsgType::Hello, {announced, target}}) &&
                    waitForHelloOk(*candidate, target)) {
                    std::lock_guard<std::mutex> lock(mutex_);
                    const auto it = peers_.find(target);
                    if (it != peers_.end()) {
                        if (it->second.connection == nullptr) {
                            it->second.connection = std::move(candidate);
                            pushEvent(Event::Kind::Join, target, "", 0);
                        }
                        it->second.dialing = false;
                    }
                    done = true;
                    break;
                }
            }
            if (attempt + 1 < kDialAttempts) {
                std::this_thread::sleep_for(std::chrono::milliseconds(250));
                std::lock_guard<std::mutex> lock(mutex_);
                const auto it = peers_.find(target);
                if (it == peers_.end() || it->second.connection != nullptr || !it->second.dialing) {
                    done = true;
                }
            }
        }

        if (!done) {
            std::lock_guard<std::mutex> lock(mutex_);
            const auto it = peers_.find(target);
            if (it != peers_.end() && it->second.connection == nullptr && it->second.dialing) {
                it->second.dialing = false;
                pushEvent(Event::Kind::Note, target, "could not reach " + target + " directly", 0);
            }
        }
    }
}

void PeerNetwork::drainPending() {
    for (std::size_t index = pending_.size(); index-- > 0;) {
        Connection& connection = *pending_[index];
        std::string remote;
        bool drop = false;

        Message message;
        while (connection.poll(message)) {
            if (remote.empty()) {
                if (message.type != MsgType::Hello || message.fields.size() < 2 ||
                    sanitizeName(message.fields[1]) != myName_) {
                    drop = true;
                    break;
                }
                remote = sanitizeName(message.fields[0]);
                if (remote.empty() || remote == myName_) {
                    remote.clear();
                    drop = true;
                    break;
                }
                const auto existing = peers_.find(remote);
                if (existing != peers_.end() && existing->second.connection != nullptr) {
                    remote.clear();
                    drop = true;
                    break;
                }
                if (!connection.send(Message {MsgType::HelloOk, {myName_}})) {
                    remote.clear();
                    drop = true;
                    break;
                }
                if (existing != peers_.end()) {
                    existing->second.dialing = false;
                    existing->second.connection = std::move(pending_[index]);
                } else {
                    Peer peer;
                    peer.name = remote;
                    peer.connection = std::move(pending_[index]);
                    peers_.emplace(remote, std::move(peer));
                }
                pushEvent(Event::Kind::Join, remote, "", 0);
                continue;
            }
            if (message.type == MsgType::PeerChat && message.fields.size() >= 4) {
                std::int64_t timestamp = 0;
                const std::string body = sanitizeBody(message.fields[2]);
                const std::string color = sanitizeBody(message.fields[3]);
                if (parseInt64(message.fields[1], timestamp) && timestamp > 0 && !body.empty() &&
                    isValidColor(color)) {
                    pushEvent(Event::Kind::Chat, remote, body, timestamp, color);
                }
            }
        }

        if (remote.empty() && !drop && connection.failed()) {
            drop = true;
        }
        if (!remote.empty() || drop) {
            pending_.erase(pending_.begin() + static_cast<std::ptrdiff_t>(index));
        }
    }
}

void PeerNetwork::drainPeers() {
    for (auto& entry : peers_) {
        Peer& peer = entry.second;
        if (peer.connection == nullptr) {
            continue;
        }
        Message message;
        while (peer.connection->poll(message)) {
            if (message.type == MsgType::PeerChat && message.fields.size() >= 4) {
                std::int64_t timestamp = 0;
                const std::string body = sanitizeBody(message.fields[2]);
                const std::string color = sanitizeBody(message.fields[3]);
                if (parseInt64(message.fields[1], timestamp) && timestamp > 0 && !body.empty() &&
                    isValidColor(color)) {
                    pushEvent(Event::Kind::Chat, peer.name, body, timestamp, color);
                }
            }
        }
        if (peer.connection->failed()) {
            pushEvent(Event::Kind::Leave, peer.name, "", 0);
            peer.connection.reset();
            if (shouldDial(peer) && !peer.dialing) {
                peer.dialing = true;
                enqueueDial(peer.name);
            }
        }
    }
}

bool PeerNetwork::shouldDial(const Peer& peer) const {
    if (passive_ || peer.connection != nullptr || peer.dialing || myName_.empty() ||
        peer.name.empty() || peer.name == myName_) {
        return false;
    }
    // Two peers that are symmetric (both advertised or both not) fall back to
    // the name rule so exactly one dials. If only one is publicly reachable,
    // the unreachable side must be the one to call out, or the link can never
    // form over the internet.
    if (myAdvertised_ == peer.advertised) {
        return myName_ < peer.name;
    }
    return peer.advertised;
}

void PeerNetwork::scanPeersForDialsLocked() {
    for (auto& entry : peers_) {
        Peer& peer = entry.second;
        if (peer.connection == nullptr && !peer.dialing && shouldDial(peer)) {
            peer.dialing = true;
            enqueueDial(peer.name);
        }
    }
}

void PeerNetwork::enqueueDial(const std::string& name) {
    dialQueue_.push_back(name);
    dialCv_.notify_all();
}

void PeerNetwork::pushEvent(Event::Kind kind, const std::string& name, const std::string& body,
                            std::int64_t timestamp, const std::string& color) {
    if (events_.size() >= kMaxQueuedEvents) {
        events_.pop_front();
    }
    Event event;
    event.kind = kind;
    event.timestamp = timestamp;
    event.name = name;
    event.body = body;
    event.color = color;
    events_.push_back(std::move(event));
}

}  // namespace chat
