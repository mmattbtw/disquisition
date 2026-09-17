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
    for (auto& entry : peers_) {
        Peer& peer = entry.second;
        if (peer.connection == nullptr && !peer.dialing && myName_ < peer.name) {
            peer.dialing = true;
            enqueueDial(peer.name);
        }
    }
}

void PeerNetwork::addPeer(const std::string& name, const std::string& host, std::uint16_t port) {
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
        existing->second.host = host;
        existing->second.port = port;
        return;
    }
    Peer peer;
    peer.name = cleaned;
    peer.host = host;
    peer.port = port;
    peer.dialing = !myName_.empty() && myName_ < cleaned;
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

void PeerNetwork::sendChat(std::int64_t timestamp, const std::string& body) {
    const Message message {MsgType::PeerChat, {std::to_string(timestamp), body}};
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
                candidate->send(Message {MsgType::Hello, {announced}});
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
                if (message.type != MsgType::Hello || message.fields.empty()) {
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
            if (message.type == MsgType::PeerChat && message.fields.size() >= 2) {
                std::int64_t timestamp = 0;
                parseInt64(message.fields[0], timestamp);
                pushEvent(Event::Kind::Chat, remote, sanitizeBody(message.fields[1]), timestamp);
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
            if (message.type == MsgType::PeerChat && message.fields.size() >= 2) {
                std::int64_t timestamp = 0;
                parseInt64(message.fields[0], timestamp);
                pushEvent(Event::Kind::Chat, peer.name, sanitizeBody(message.fields[1]), timestamp);
            }
        }
        if (peer.connection->failed()) {
            pushEvent(Event::Kind::Leave, peer.name, "", 0);
            peer.connection.reset();
            if (!myName_.empty() && myName_ < peer.name && !peer.dialing) {
                peer.dialing = true;
                enqueueDial(peer.name);
            }
        }
    }
}

void PeerNetwork::enqueueDial(const std::string& name) {
    dialQueue_.push_back(name);
    dialCv_.notify_all();
}

void PeerNetwork::pushEvent(Event::Kind kind, const std::string& name, const std::string& body,
                            std::int64_t timestamp) {
    if (events_.size() >= kMaxQueuedEvents) {
        events_.pop_front();
    }
    Event event;
    event.kind = kind;
    event.timestamp = timestamp;
    event.name = name;
    event.body = body;
    events_.push_back(std::move(event));
}

}  // namespace chat
