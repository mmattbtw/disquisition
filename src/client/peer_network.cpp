#include "client/peer_network.h"

#include <unistd.h>

#include <chrono>
#include <thread>

#include "common/net.h"
#include "common/protocol.h"
#include "util/log.h"

namespace chat {
namespace {

using Clock = std::chrono::steady_clock;

constexpr std::size_t kMaxPendingInbound = 16;
constexpr std::size_t kMaxQueuedEvents = 512;
constexpr int kDialAttempts = 3;
constexpr int kDialTimeoutMs = 3000;
constexpr int kDialRetryDelayMs = 250;
constexpr int kHandshakePollMs = 10;

bool waitForHelloOk(Connection& link, const std::string& target) {
    const auto deadline = Clock::now() + std::chrono::milliseconds(kDialTimeoutMs);
    while (!link.failed() && Clock::now() < deadline) {
        Message reply;
        if (link.poll(reply)) {
            return reply.type == MsgType::HelloOk && !reply.fields.empty() &&
                   sanitizeName(reply.fields[0]) == target;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(kHandshakePollMs));
    }
    return false;
}

// Connects to a peer and completes the Hello handshake.
std::unique_ptr<Connection> dialOnce(const std::string& host, std::uint16_t port,
                                     const std::string& myName, const std::string& target) {
    auto link = std::make_unique<Connection>();
    std::string error;
    if (!link->connectTo(host, port, kDialTimeoutMs, error)) {
        return nullptr;
    }
    link->startReader();
    if (!link->send(Message{MsgType::Hello, {myName, target}}) || !waitForHelloOk(*link, target)) {
        return nullptr;
    }
    return link;
}

} // namespace

PeerNetwork::~PeerNetwork() {
    stop();
}

bool PeerNetwork::start(std::uint16_t port, std::string& error) {
    if (listenFd_ >= 0 || running_.load()) {
        error = "peer network already started";
        return false;
    }
    listenFd_ = listenTcp(port, error);
    if (listenFd_ < 0) {
        return false;
    }
    listenPort_ = localPort(listenFd_);
    if (listenPort_ == 0) {
        listenPort_ = port;
    }

    running_.store(true);
    acceptThread_ = std::thread([this] { acceptLoop(); });
    dialThread_ = std::thread([this] { dialLoop(); });
    return true;
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

void PeerNetwork::stop() {
    if (!running_.exchange(false)) {
        return;
    }
    dialCv_.notify_all();
    for (std::thread* thread : {&acceptThread_, &dialThread_}) {
        if (thread->joinable()) {
            thread->join();
        }
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
    dialEligiblePeers();
}

void PeerNetwork::setMyAdvertised(bool advertised) {
    std::lock_guard<std::mutex> lock(mutex_);
    myAdvertised_ = advertised;
    dialEligiblePeers();
}

void PeerNetwork::setPassive(bool passive) {
    std::lock_guard<std::mutex> lock(mutex_);
    passive_ = passive;
    if (!passive_) {
        dialEligiblePeers();
        return;
    }
    dialQueue_.clear();
    for (auto& [name, peer] : peers_) {
        peer.dialing = false;
        peer.connection.reset();
    }
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
    Peer& peer = peers_[cleaned];
    peer.name = cleaned;
    peer.host = host;
    peer.port = port;
    peer.advertised = advertised;
    if (shouldDial(peer)) {
        peer.dialing = true;
        enqueueDial(peer.name);
    }
}

void PeerNetwork::removePeer(const std::string& name) {
    std::lock_guard<std::mutex> lock(mutex_);
    peers_.erase(name);
}

void PeerNetwork::adoptInbound(const std::string& name, std::unique_ptr<Connection> connection) {
    if (connection == nullptr) {
        return;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    attachInbound(sanitizeName(name), std::move(connection));
}

bool PeerNetwork::connectedTo(const std::string& name) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = peers_.find(name);
    return it != peers_.end() && it->second.connection != nullptr;
}

bool PeerNetwork::peerAddress(const std::string& name, std::string& host,
                              std::uint16_t& port) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = peers_.find(name);
    if (it == peers_.end()) {
        return false;
    }
    host = it->second.host;
    port = it->second.port;
    return true;
}

bool PeerNetwork::settled() const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!dialQueue_.empty() || !pending_.empty()) {
        return false;
    }
    for (const auto& [name, peer] : peers_) {
        if (peer.dialing) {
            return false;
        }
    }
    return true;
}

void PeerNetwork::sendChat(std::int64_t timestamp, const std::string& body,
                           const std::string& color) {
    std::lock_guard<std::mutex> lock(mutex_);
    const Message message{MsgType::PeerChat, {myName_, std::to_string(timestamp), body, color}};
    for (auto& [name, peer] : peers_) {
        if (peer.connection != nullptr) {
            peer.connection->send(message);
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
    acceptConnections(listenFd_, running_, [this](int fd) {
        auto connection = Connection::fromAccepted(fd);
        if (connection == nullptr) {
            return;
        }
        std::lock_guard<std::mutex> lock(mutex_);
        if (pending_.size() >= kMaxPendingInbound) {
            pending_.erase(pending_.begin());
        }
        pending_.push_back(std::move(connection));
    });
}

void PeerNetwork::dialLoop() {
    DialTarget target;
    while (nextDialTarget(target)) {
        finishDial(target.name, dialWithRetries(target));
    }
}

bool PeerNetwork::nextDialTarget(DialTarget& target) {
    std::unique_lock<std::mutex> lock(mutex_);
    for (;;) {
        dialCv_.wait(lock, [this] { return !running_.load() || !dialQueue_.empty(); });
        if (!running_.load()) {
            return false;
        }
        const std::string name = dialQueue_.front();
        dialQueue_.pop_front();
        if (isAwaitingDial(name)) {
            const Peer& peer = peers_.at(name);
            target = DialTarget{name, peer.host, peer.port, myName_};
            return true;
        }
    }
}

std::unique_ptr<Connection> PeerNetwork::dialWithRetries(const DialTarget& target) {
    for (int attempt = 1; attempt <= kDialAttempts && running_.load(); ++attempt) {
        if (auto link = dialOnce(target.host, target.port, target.myName, target.name)) {
            return link;
        }
        LOG_DEBUG("dial {} at {}:{} failed (attempt {} of {})", target.name, target.host,
                  target.port, attempt, kDialAttempts);
        if (attempt == kDialAttempts) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(kDialRetryDelayMs));
        // The peer may have left, or called us, while we waited.
        std::lock_guard<std::mutex> lock(mutex_);
        if (!isAwaitingDial(target.name)) {
            break;
        }
    }
    return nullptr;
}

void PeerNetwork::finishDial(const std::string& name, std::unique_ptr<Connection> link) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = peers_.find(name);
    if (it == peers_.end()) {
        return;
    }
    Peer& peer = it->second;
    if (link == nullptr) {
        if (peer.dialing && peer.connection == nullptr) {
            peer.dialing = false;
            LOG_DEBUG("giving up on {}", name);
            pushEvent(Event::Kind::Note, name, "could not reach " + name + " directly");
        }
        return;
    }
    if (passive_) {
        return;
    }
    if (peer.connection == nullptr) {
        peer.connection = std::move(link);
        LOG_DEBUG("linked to {} (outbound)", name);
        pushEvent(Event::Kind::Join, name);
    }
    peer.dialing = false;
}

bool PeerNetwork::shouldDial(const Peer& peer) const {
    if (passive_ || peer.connection != nullptr || peer.dialing || myName_.empty() ||
        peer.name.empty() || peer.name == myName_) {
        return false;
    }
    // An unreachable peer cannot be dialled, so it must dial out. Between
    // equals, the name that sorts first dials.
    if (myAdvertised_ == peer.advertised) {
        return myName_ < peer.name;
    }
    return peer.advertised;
}

bool PeerNetwork::isAwaitingDial(const std::string& name) const {
    const auto it = peers_.find(name);
    return it != peers_.end() && it->second.connection == nullptr && it->second.dialing;
}

void PeerNetwork::dialEligiblePeers() {
    for (auto& [name, peer] : peers_) {
        if (shouldDial(peer)) {
            peer.dialing = true;
            enqueueDial(name);
        }
    }
}

void PeerNetwork::enqueueDial(const std::string& name) {
    dialQueue_.push_back(name);
    dialCv_.notify_all();
}

bool PeerNetwork::attachInbound(const std::string& remote, std::unique_ptr<Connection> connection) {
    if (remote.empty() || remote == myName_) {
        return false;
    }
    const auto existing = peers_.find(remote);
    if (existing != peers_.end() && existing->second.connection != nullptr) {
        LOG_DEBUG("refused second link from {}", remote);
        return false; // one link per pair: keep the existing one
    }
    if (!connection->send(Message{MsgType::HelloOk, {myName_}})) {
        return false;
    }
    Peer& peer = peers_[remote];
    peer.name = remote;
    peer.dialing = false;
    peer.connection = std::move(connection);
    LOG_DEBUG("linked to {} (inbound)", remote);
    pushEvent(Event::Kind::Join, remote);
    return true;
}

// Each inbound connection must open with a Hello naming us as its target.
void PeerNetwork::drainPending() {
    for (std::size_t index = pending_.size(); index-- > 0;) {
        const auto erase = [&] {
            pending_.erase(pending_.begin() + static_cast<std::ptrdiff_t>(index));
        };

        Message hello;
        if (!pending_[index]->poll(hello)) {
            if (pending_[index]->failed()) {
                erase();
            }
            continue;
        }
        if (hello.type != MsgType::Hello || hello.fields.size() < 2 ||
            sanitizeName(hello.fields[1]) != myName_) {
            LOG_DEBUG("refused inbound connection: bad Hello");
            erase();
            continue;
        }
        std::unique_ptr<Connection> connection = std::move(pending_[index]);
        erase();
        attachInbound(sanitizeName(hello.fields[0]), std::move(connection));
    }
}

void PeerNetwork::drainPeers() {
    for (auto& [name, peer] : peers_) {
        if (peer.connection == nullptr) {
            continue;
        }
        Message message;
        while (peer.connection->poll(message)) {
            // Trust the name the link was established with, not the frame.
            ChatPayload payload;
            if (parsePeerChat(message, payload)) {
                pushEvent(Event::Kind::Chat, peer.name, payload.body, payload.timestamp,
                          payload.color);
            }
        }
        if (peer.connection->failed()) {
            LOG_DEBUG("link to {} dropped", peer.name);
            pushEvent(Event::Kind::Leave, peer.name);
            peer.connection.reset();
            if (shouldDial(peer)) {
                peer.dialing = true;
                enqueueDial(peer.name);
            }
        }
    }
}

void PeerNetwork::pushEvent(Event::Kind kind, const std::string& name, const std::string& body,
                            std::int64_t timestamp, const std::string& color) {
    if (events_.size() >= kMaxQueuedEvents) {
        events_.pop_front();
    }
    events_.push_back(Event{kind, timestamp, name, body, color});
}

} // namespace chat
