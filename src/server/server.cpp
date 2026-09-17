#include "server/server.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <csignal>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <stdexcept>
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

    // A client vanishing mid-write must not kill the whole server.
    std::signal(SIGPIPE, SIG_IGN);
}

std::int64_t nowSeconds() {
    return static_cast<std::int64_t>(std::time(nullptr));
}

std::string trim(const std::string& text) {
    const auto begin = text.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos) {
        return "";
    }
    const auto end = text.find_last_not_of(" \t\r\n");
    return text.substr(begin, end - begin + 1);
}

// A peer's advertised address is just relayed to whoever wants to dial it, so
// we only clean it enough that it cannot smuggle stray bytes into a frame.
std::string sanitizeHost(const std::string& host) {
    std::string cleaned = trim(host);
    constexpr std::size_t kMaxHostLength = 253;
    if (cleaned.size() > kMaxHostLength) {
        cleaned.resize(kMaxHostLength);
    }
    for (char& character : cleaned) {
        const unsigned char value = static_cast<unsigned char>(character);
        if (value < 0x20 || value == 0x7F) {
            character = '?';
        }
    }
    return cleaned;
}

std::string numericHost(const sockaddr_storage& address) {
    char host[NI_MAXHOST] = {0};
    if (getnameinfo(reinterpret_cast<const sockaddr*>(&address), sizeof(address), host, sizeof(host),
                    nullptr, 0, NI_NUMERICHOST) != 0) {
        return "unknown";
    }
    std::string result = host;
    // Dual-stack listeners report IPv4 peers as ::ffff:1.2.3.4; dialling the
    // bare IPv4 address avoids surprising anyone parsing the announcement.
    const std::string mapped = "::ffff:";
    if (result.rfind(mapped, 0) == 0 && result.find(':', mapped.size()) == std::string::npos) {
        result.erase(0, mapped.size());
    }
    return result;
}

std::string describePeer(const sockaddr_storage& address) {
    char service[NI_MAXSERV] = {0};
    if (getnameinfo(reinterpret_cast<const sockaddr*>(&address), sizeof(address), nullptr, 0, service,
                    sizeof(service), NI_NUMERICSERV) != 0) {
        return "unknown";
    }
    return numericHost(address) + ":" + service;
}

bool setNonBlocking(int fd) {
    const int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) {
        return false;
    }
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

}  // namespace

Server::Server(ServerOptions options, Database& database)
    : options_(std::move(options)), database_(database) {}

void Server::listen() {
    installSignalHandlers();

    addrinfo hints {};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;

    const std::string service = std::to_string(options_.port);
    addrinfo* results = nullptr;
    const int rc = getaddrinfo(nullptr, service.c_str(), &hints, &results);
    if (rc != 0) {
        throw std::runtime_error(std::string("cannot resolve bind address: ") + gai_strerror(rc));
    }

    for (addrinfo* entry = results; entry != nullptr; entry = entry->ai_next) {
        const int fd = socket(entry->ai_family, entry->ai_socktype, entry->ai_protocol);
        if (fd < 0) {
            continue;
        }

        const int enable = 1;
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(enable));

        if (bind(fd, entry->ai_addr, entry->ai_addrlen) == 0 && ::listen(fd, SOMAXCONN) == 0) {
            listenFd_ = fd;
            break;
        }

        ::close(fd);
    }
    freeaddrinfo(results);

    if (listenFd_ < 0) {
        throw std::runtime_error("cannot bind port " + service);
    }
    if (!setNonBlocking(listenFd_)) {
        throw std::runtime_error("cannot make listening socket non-blocking");
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
}

void Server::run() {
    log("listening on port " + std::to_string(boundPort_));

    while (gStopRequested == 0) {
        std::vector<pollfd> fds;
        fds.reserve(connections_.size() + 1);
        fds.push_back(pollfd {listenFd_, POLLIN, 0});
        for (const Connection& connection : connections_) {
            short events = connection.closing ? 0 : POLLIN;
            if (!connection.out.empty()) {
                events |= POLLOUT;
            }
            fds.push_back(pollfd {connection.fd, events, 0});
        }

        const int ready = poll(fds.data(), static_cast<nfds_t>(fds.size()), 500);
        if (ready < 0) {
            if (errno == EINTR) {
                continue;
            }
            throw std::runtime_error(std::string("poll failed: ") + std::strerror(errno));
        }
        if (ready == 0) {
            continue;
        }

        if ((fds[0].revents & POLLIN) != 0) {
            acceptClients();
        }

        // Walk backwards so erasing a connection cannot invalidate the loop.
        for (std::size_t index = connections_.size(); index-- > 0;) {
            if (index + 1 >= fds.size()) {
                continue;
            }
            const short revents = fds[index + 1].revents;
            if (revents == 0) {
                continue;
            }

            bool alive = true;
            if ((revents & POLLIN) != 0 && !connections_[index].closing) {
                alive = readFrom(connections_[index]) && handleFrames(connections_[index]);
            }
            if (alive && !connections_[index].out.empty() && (revents & POLLOUT) != 0) {
                alive = writeTo(connections_[index]);
            }
            // A connection asked to close is dropped once its last words are out.
            if (alive && connections_[index].closing && connections_[index].out.empty()) {
                alive = false;
            }
            if (!alive) {
                dropConnection(index);
            }
        }
    }

    log("shutting down");
    for (std::size_t index = connections_.size(); index-- > 0;) {
        dropConnection(index);
    }
}

void Server::acceptClients() {
    for (;;) {
        sockaddr_storage address {};
        socklen_t length = sizeof(address);
        const int fd = accept(listenFd_, reinterpret_cast<sockaddr*>(&address), &length);
        if (fd < 0) {
            if (errno == EINTR) {
                continue;
            }
            return;
        }

        if (!setNonBlocking(fd)) {
            ::close(fd);
            continue;
        }

        Connection connection;
        connection.fd = fd;
        connection.host = numericHost(address);
        if (connections_.size() >= options_.maxClients) {
            // Queue the rejection so the event loop can flush it and close.
            connection.out = encode(Message {MsgType::Error, {"server is full"}});
            connection.closing = true;
            log("rejected " + describePeer(address) + ": server is full");
            connections_.push_back(std::move(connection));
            continue;
        }

        connections_.push_back(std::move(connection));
        log("connection from " + describePeer(address));
    }
}

bool Server::readFrom(Connection& connection) {
    // One recv per poll event: partial frames are drained by handleFrames and
    // level-triggered poll brings us straight back for whatever is left.
    char buffer[4096];
    const ssize_t bytes = recv(connection.fd, buffer, sizeof(buffer), 0);
    if (bytes > 0) {
        connection.in.append(buffer, static_cast<std::size_t>(bytes));
        return true;
    }
    if (bytes == 0) {
        return false;
    }
    if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) {
        return true;
    }
    return false;
}

bool Server::writeTo(Connection& connection) {
    while (!connection.out.empty()) {
        const ssize_t bytes = ::send(connection.fd, connection.out.data(), connection.out.size(), 0);
        if (bytes > 0) {
            connection.out.erase(0, static_cast<std::size_t>(bytes));
            continue;
        }
        if (bytes < 0 && errno == EINTR) {
            continue;
        }
        if (bytes < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            return true;
        }
        return false;
    }
    return true;
}

bool Server::handleFrames(Connection& connection) {
    for (;;) {
        Message message;
        const DecodeStatus status = decode(connection.in, message);
        if (status == DecodeStatus::Incomplete) {
            // A peer dribbling bytes towards a frame that never lands gets cut.
            return connection.in.size() <= kMaxFrameSize * 2;
        }
        if (status == DecodeStatus::Malformed) {
            return false;
        }

        switch (message.type) {
            case MsgType::Login:
                handleLogin(connection, message);
                break;
            case MsgType::Store:
                handleStore(connection, message);
                break;
            case MsgType::FetchHistory:
                handleFetchHistory(connection);
                break;
            default:
                reject(connection, "unexpected message");
                break;
        }
        if (connection.closing) {
            return true;
        }
    }
}

void Server::handleLogin(Connection& connection, const Message& message) {
    if (connection.authenticated) {
        reject(connection, "already signed in");
        return;
    }
    if (message.fields.size() < 2) {
        reject(connection, "login needs a name and a peer port");
        return;
    }

    std::int64_t peerPort = 0;
    if (!parseInt64(message.fields[1], peerPort) || peerPort < 1 || peerPort > 65535) {
        reject(connection, "invalid peer port");
        return;
    }

    const std::string requested = sanitizeName(message.fields[0]);
    if (requested.empty()) {
        reject(connection, "please pick a name");
        return;
    }

    connection.name = uniqueName(requested);
    connection.peerPort = static_cast<std::uint16_t>(peerPort);
    connection.advertisedHost = sanitizeHost(message.fields.size() >= 3 ? message.fields[2] : "");
    connection.authenticated = true;

    send(connection, Message {MsgType::LoginOk, {connection.name, "welcome, " + connection.name}});

    // The new peer learns every on-line peer first so it can dial out; the
    // others then learn about the new arrival.
    for (const Connection& other : connections_) {
        if (&other != &connection && other.authenticated) {
            send(connection, Message {MsgType::Peer,
                                      {other.name, hostFor(other), std::to_string(other.peerPort),
                                       other.advertisedHost.empty() ? "0" : "1"}});
        }
    }

    log(connection.name + " joined from " + hostFor(connection));
    broadcast(Message {MsgType::PeerJoined,
                       {connection.name, hostFor(connection), std::to_string(connection.peerPort),
                        connection.advertisedHost.empty() ? "0" : "1"}},
              &connection);
    broadcastUsers();
}

void Server::handleStore(Connection& connection, const Message& message) {
    if (!connection.authenticated) {
        reject(connection, "sign in first");
        return;
    }
    if (message.fields.size() < 2) {
        reject(connection, "store needs a timestamp and a body");
        return;
    }

    std::int64_t timestamp = 0;
    if (!parseInt64(message.fields[0], timestamp) || timestamp <= 0) {
        reject(connection, "invalid timestamp");
        return;
    }

    const std::string body = sanitizeBody(message.fields[1]);
    if (body.empty()) {
        return;
    }

    database_.add(timestamp, connection.name, body);
    log(connection.name + " (stored): " + body);
}

void Server::handleFetchHistory(Connection& connection) {
    if (!connection.authenticated) {
        reject(connection, "sign in first");
        return;
    }
    sendHistory(connection);
}

void Server::sendHistory(Connection& connection) {
    const std::vector<StoredMessage> history = database_.recent(options_.historyLimit);
    for (const StoredMessage& stored : history) {
        send(connection, Message {MsgType::History,
                                  {std::to_string(stored.timestamp), stored.sender, stored.body}});
    }
    send(connection, Message {MsgType::HistoryEnd, {}});
}

void Server::reject(Connection& connection, const std::string& reason) {
    // Queue the reason and stop reading. Closing immediately could turn into a
    // TCP reset that throws away the very bytes we want the client to see.
    send(connection, Message {MsgType::Error, {reason}});
    connection.closing = true;
}

void Server::send(Connection& connection, const Message& message) {
    connection.out.append(encode(message));
}

void Server::broadcast(const Message& message, const Connection* except) {
    const std::string frame = encode(message);
    for (Connection& connection : connections_) {
        if (!connection.authenticated || &connection == except) {
            continue;
        }
        connection.out.append(frame);
    }
}

void Server::broadcastUsers() {
    Message message {MsgType::Users, {}};
    for (const Connection& connection : connections_) {
        if (connection.authenticated) {
            message.fields.push_back(connection.name);
        }
    }
    broadcast(message);
}

std::string Server::uniqueName(const std::string& requested) const {
    auto taken = [this](const std::string& candidate) {
        for (const Connection& connection : connections_) {
            if (connection.authenticated && connection.name == candidate) {
                return true;
            }
        }
        return false;
    };

    if (!taken(requested)) {
        return requested;
    }
    for (int suffix = 2; suffix < 1000; ++suffix) {
        const std::string candidate = requested + "-" + std::to_string(suffix);
        if (!taken(candidate)) {
            return candidate;
        }
    }
    return requested + "-" + std::to_string(nowSeconds());
}

const std::string& Server::hostFor(const Connection& connection) const {
    return connection.advertisedHost.empty() ? connection.host : connection.advertisedHost;
}

void Server::dropConnection(std::size_t index) {
    Connection& connection = connections_[index];
    const std::string name = connection.name;
    const bool wasAuthenticated = connection.authenticated;

    if (connection.fd >= 0) {
        ::close(connection.fd);
    }
    connections_.erase(connections_.begin() + static_cast<std::ptrdiff_t>(index));

    if (wasAuthenticated) {
        log(name + " left");
        broadcast(Message {MsgType::PeerLeft, {name}});
        broadcastUsers();
    }
}

void Server::log(const std::string& text) const {
    std::printf("[server] %s\n", text.c_str());
    std::fflush(stdout);
}

}  // namespace chat
