#include "server/server.h"

#include <netdb.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <exception>
#include <stdexcept>
#include <utility>

#include "common/net.h"
#include "server/database.h"
#include "util/log.h"

namespace chat {
namespace {

constexpr int kPollTimeoutMs = 500;
constexpr std::size_t kMaxHostLength = 253;

// Advertised hosts are passed on verbatim, so only strip what could corrupt
// a frame or a terminal.
std::string sanitizeHost(const std::string& host) {
    std::string cleaned = trim(host);
    if (cleaned.size() > kMaxHostLength) {
        cleaned.resize(kMaxHostLength);
    }
    for (char& character : cleaned) {
        const auto value = static_cast<unsigned char>(character);
        if (value < 0x20 || value == 0x7F) {
            character = '?';
        }
    }
    return cleaned;
}

// `length` must be the one accept() returned; macOS rejects any other.
std::string numericHost(const sockaddr_storage& address, socklen_t length) {
    char host[NI_MAXHOST] = {};
    if (getnameinfo(reinterpret_cast<const sockaddr*>(&address), length, host, sizeof(host),
                    nullptr, 0, NI_NUMERICHOST) != 0) {
        return "unknown";
    }
    // Report IPv4 clients of a dual-stack socket as plain IPv4.
    std::string result = host;
    const std::string mapped = "::ffff:";
    if (result.rfind(mapped, 0) == 0 && result.find(':', mapped.size()) == std::string::npos) {
        result.erase(0, mapped.size());
    }
    return result;
}

std::string describePeer(const sockaddr_storage& address, socklen_t length) {
    char service[NI_MAXSERV] = {};
    if (getnameinfo(reinterpret_cast<const sockaddr*>(&address), length, nullptr, 0, service,
                    sizeof(service), NI_NUMERICSERV) != 0) {
        return "unknown";
    }
    return numericHost(address, length) + ":" + service;
}

bool wouldBlock() {
    return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR;
}

} // namespace

int runServer(const ServerOptions& options) {
    logToConsole("server");
    try {
        Database database(options.databasePath);
        Server server(options);
        server.listen();
        server.run();
        return 0;
    }
    catch (const std::exception& error) {
        LOG_ERR("server error: {}", error.what());
        return 1;
    }
}

Server::Server(ServerOptions options) : options_(std::move(options)) {
}

Server::~Server() {
    for (const Client& client : clients_) {
        ::close(client.fd);
    }
    if (listenFd_ >= 0) {
        ::close(listenFd_);
    }
}

void Server::listen() {
    ignoreSigpipe();
    installShutdownHandlers();

    std::string error;
    listenFd_ = listenTcp(options_.port, error);
    if (listenFd_ < 0) {
        throw std::runtime_error("cannot listen on port " + std::to_string(options_.port) + ": " +
                                 error);
    }
    if (!setBlocking(listenFd_, false)) {
        throw std::runtime_error("cannot make listening socket non-blocking");
    }
    boundPort_ = localPort(listenFd_);
    if (boundPort_ == 0) {
        boundPort_ = options_.port;
    }
}

void Server::run() {
    LOG_INFO("listening on port {}", boundPort_);

    while (!shutdownRequested()) {
        std::vector<pollfd> fds = pollSet();
        const int ready = ::poll(fds.data(), static_cast<nfds_t>(fds.size()), kPollTimeoutMs);
        if (ready < 0 && errno != EINTR) {
            throw std::runtime_error(std::string("poll failed: ") + std::strerror(errno));
        }
        if (ready <= 0) {
            continue;
        }

        if ((fds[0].revents & POLLIN) != 0) {
            acceptClients();
        }
        // fds[i + 1] belongs to clients_[i]. Newly accepted clients were not
        // polled, and walking backwards keeps indexes valid across drops.
        const std::size_t polled = fds.size() - 1;
        for (std::size_t index = polled; index-- > 0;) {
            const short revents = fds[index + 1].revents;
            if (revents != 0 && !service(clients_[index], revents)) {
                dropClient(index);
            }
        }
    }

    LOG_INFO("shutting down");
    while (!clients_.empty()) {
        dropClient(clients_.size() - 1);
    }
}

std::vector<pollfd> Server::pollSet() const {
    std::vector<pollfd> fds;
    fds.reserve(clients_.size() + 1);
    fds.push_back(pollfd{listenFd_, POLLIN, 0});
    for (const Client& client : clients_) {
        short events = client.closing ? 0 : POLLIN;
        if (!client.out.empty()) {
            events |= POLLOUT;
        }
        fds.push_back(pollfd{client.fd, events, 0});
    }
    return fds;
}

void Server::acceptClients() {
    for (;;) {
        sockaddr_storage address{};
        socklen_t length = sizeof(address);
        const int fd = accept(listenFd_, reinterpret_cast<sockaddr*>(&address), &length);
        if (fd < 0) {
            if (errno == EINTR) {
                continue;
            }
            return;
        }
        if (!setBlocking(fd, false)) {
            ::close(fd);
            continue;
        }

        Client client;
        client.fd = fd;
        client.host = numericHost(address, length);
        LOG_INFO("connection from {}", describePeer(address, length));
        if (clients_.size() >= options_.maxClients) {
            reject(client, "server is full");
        }
        clients_.push_back(std::move(client));
    }
}

// Returns false when the client should be dropped.
bool Server::service(Client& client, short revents) {
    if ((revents & (POLLIN | POLLHUP)) != 0 && !client.closing) {
        if (!readFrom(client) || !handleFrames(client)) {
            return false;
        }
    }
    if ((revents & POLLOUT) != 0 && !client.out.empty() && !writeTo(client)) {
        return false;
    }
    if ((revents & (POLLERR | POLLNVAL)) != 0) {
        return false;
    }
    return !(client.closing && client.out.empty());
}

bool Server::readFrom(Client& client) {
    char buffer[4096];
    const ssize_t bytes = recv(client.fd, buffer, sizeof(buffer), 0);
    if (bytes > 0) {
        client.in.append(buffer, static_cast<std::size_t>(bytes));
        return true;
    }
    if (bytes == 0) {
        // Half-closed: still answer what was already sent, then drop.
        client.closing = true;
        return true;
    }
    return wouldBlock();
}

bool Server::writeTo(Client& client) {
    while (!client.out.empty()) {
        const ssize_t bytes = ::send(client.fd, client.out.data(), client.out.size(), 0);
        if (bytes > 0) {
            client.out.erase(0, static_cast<std::size_t>(bytes));
            continue;
        }
        if (bytes < 0 && errno == EINTR) {
            continue;
        }
        // A full socket buffer is fine: poll() reports when it drains.
        return bytes < 0 && (errno == EAGAIN || errno == EWOULDBLOCK);
    }
    return true;
}

bool Server::handleFrames(Client& client) {
    for (;;) {
        Message message;
        const DecodeStatus status = decode(client.in, message);
        if (status == DecodeStatus::Incomplete) {
            // Cut off clients that trickle bytes without ever completing a frame.
            if (client.in.size() > kMaxFrameSize * 2) {
                LOG_WARN("dropping {}: frame never completed", describe(client));
                return false;
            }
            return true;
        }
        if (status == DecodeStatus::Malformed) {
            LOG_WARN("dropping {}: malformed frame", describe(client));
            return false;
        }

        switch (message.type) {
            case MsgType::Login:
                handleLogin(client, message);
                break;
            case MsgType::Store:
                handleStore(client, message);
                break;
            case MsgType::FetchHistory:
                handleFetchHistory(client);
                break;
            default:
                reject(client, "unexpected message");
                break;
        }
        if (client.closing) {
            return true;
        }
    }
}

void Server::handleLogin(Client& client, const Message& message) {
    if (client.authenticated) {
        reject(client, "already signed in");
        return;
    }
    if (message.fields.size() < 2) {
        reject(client, "login needs a name and a peer port");
        return;
    }
    std::uint16_t peerPort = 0;
    if (!parsePort(message.fields[1], peerPort, false)) {
        reject(client, "invalid peer port");
        return;
    }
    const std::string requested = sanitizeName(message.fields[0]);
    if (requested.empty()) {
        reject(client, "please pick a name");
        return;
    }

    client.name = uniqueName(requested);
    client.peerPort = peerPort;
    client.advertisedHost = sanitizeHost(message.fields.size() >= 3 ? message.fields[2] : "");
    client.authenticated = true;
    send(client, Message{MsgType::LoginOk, {client.name, "welcome, " + client.name}});

    // The newcomer learns the roster first, then everyone learns about it.
    for (const Client& other : clients_) {
        if (&other != &client && other.authenticated) {
            send(client, peerMessage(MsgType::Peer, addressOf(other)));
        }
    }
    const PeerAddress address = addressOf(client);
    LOG_INFO("{} joined from {}", client.name, address.host);
    broadcast(peerMessage(MsgType::PeerJoined, address), &client);
    broadcastUsers();
}

void Server::handleStore(Client& client, const Message& message) {
    if (!client.authenticated) {
        reject(client, "sign in first");
        return;
    }
    if (message.fields.size() < 3) {
        reject(client, "store needs a timestamp, body, and color");
        return;
    }
    std::int64_t timestamp = 0;
    if (!parseInt64(message.fields[0], timestamp) || timestamp <= 0) {
        reject(client, "invalid timestamp");
        return;
    }
    const std::string body = sanitizeBody(message.fields[1]);
    if (body.empty()) {
        return;
    }
    const std::string color = sanitizeBody(message.fields[2]);
    if (!isValidColor(color)) {
        reject(client, "invalid message color");
        return;
    }

    // Accept legacy storage frames without retaining or logging their contents.
}

void Server::handleFetchHistory(Client& client) {
    if (!client.authenticated) {
        reject(client, "sign in first");
        return;
    }
    // Older clients still request history after signing in.
    send(client, Message{MsgType::HistoryEnd, {}});
}

// Sends the reason and closes once it is flushed. Closing straight away could
// reset the connection and discard the message.
void Server::reject(Client& client, const std::string& reason) {
    LOG_WARN("rejected {}: {}", describe(client), reason);
    send(client, Message{MsgType::Error, {reason}});
    client.closing = true;
}

void Server::send(Client& client, const Message& message) {
    client.out.append(encode(message));
}

void Server::broadcast(const Message& message, const Client* except) {
    const std::string frame = encode(message);
    for (Client& client : clients_) {
        if (client.authenticated && &client != except) {
            client.out.append(frame);
        }
    }
}

void Server::broadcastUsers() {
    Message message{MsgType::Users, {}};
    for (const Client& client : clients_) {
        if (client.authenticated) {
            message.fields.push_back(client.name);
        }
    }
    broadcast(message);
}

// Appends "-2", "-3", ... to a taken name, shortening it to stay in bounds.
std::string Server::uniqueName(const std::string& requested) const {
    const auto taken = [this](const std::string& candidate) {
        for (const Client& client : clients_) {
            if (client.authenticated && client.name == candidate) {
                return true;
            }
        }
        return false;
    };

    std::string candidate = requested;
    for (std::size_t suffix = 2; taken(candidate); ++suffix) {
        const std::string ending = "-" + std::to_string(suffix);
        candidate = requested.substr(0, kMaxNameLength - ending.size()) + ending;
    }
    return candidate;
}

PeerAddress Server::addressOf(const Client& client) const {
    const bool advertised = !client.advertisedHost.empty();
    return PeerAddress{client.name, advertised ? client.advertisedHost : client.host,
                       client.peerPort, advertised};
}

void Server::dropClient(std::size_t index) {
    const Client client = std::move(clients_[index]);
    clients_.erase(clients_.begin() + static_cast<std::ptrdiff_t>(index));
    ::close(client.fd);

    if (client.authenticated) {
        LOG_INFO("{} left", client.name);
        broadcast(Message{MsgType::PeerLeft, {client.name}});
        broadcastUsers();
    }
}

std::string Server::describe(const Client& client) {
    return client.authenticated ? client.name : client.host;
}

} // namespace chat
