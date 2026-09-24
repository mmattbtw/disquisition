#include "client/connection.h"

#include <netdb.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>

#include "common/net.h"
#include "util/log.h"

namespace chat {
namespace {

// Peers can vanish mid-write. Report that as a send error instead of letting
// SIGPIPE kill a program that embeds the client library.
#ifdef MSG_NOSIGNAL
constexpr int kSendFlags = MSG_NOSIGNAL;
#else
constexpr int kSendFlags = 0;
#endif

void suppressSigpipe([[maybe_unused]] int fd) {
#ifdef SO_NOSIGPIPE
    const int enable = 1;
    setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &enable, sizeof(enable));
#endif
}

std::string lastError() {
    return std::strerror(errno);
}

// Connects without blocking for longer than `timeoutMs` (negative: no limit).
bool connectSocket(int fd, const sockaddr* address, socklen_t length, int timeoutMs,
                   std::string& error) {
    if (timeoutMs < 0) {
        if (::connect(fd, address, length) != 0) {
            error = lastError();
            return false;
        }
        return true;
    }

    if (!setBlocking(fd, false)) {
        error = lastError();
        return false;
    }
    if (::connect(fd, address, length) != 0) {
        if (errno != EINPROGRESS) {
            error = lastError();
            return false;
        }
        pollfd watched{fd, POLLOUT, 0};
        const int ready = ::poll(&watched, 1, timeoutMs);
        if (ready <= 0) {
            error = ready == 0 ? "connection timed out" : lastError();
            return false;
        }
        int socketError = 0;
        socklen_t size = sizeof(socketError);
        if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &socketError, &size) != 0) {
            error = lastError();
            return false;
        }
        if (socketError != 0) {
            error = std::strerror(socketError);
            return false;
        }
    }
    if (!setBlocking(fd, true)) {
        error = lastError();
        return false;
    }
    return true;
}

} // namespace

Connection::~Connection() {
    stop();
}

bool Connection::connectTo(const std::string& host, std::uint16_t port, std::string& error) {
    return connectTo(host, port, -1, error);
}

bool Connection::connectTo(const std::string& host, std::uint16_t port, int timeoutMs,
                           std::string& error) {
    if (fd_.load() >= 0) {
        error = "already connected";
        return false;
    }

    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* results = nullptr;
    const int rc = getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &results);
    if (rc != 0) {
        error = gai_strerror(rc);
        failed_.store(true);
        return false;
    }

    error = "could not connect";
    int connected = -1;
    for (addrinfo* entry = results; entry != nullptr && connected < 0; entry = entry->ai_next) {
        const int fd = socket(entry->ai_family, entry->ai_socktype, entry->ai_protocol);
        if (fd < 0) {
            error = lastError();
        }
        else if (connectSocket(fd, entry->ai_addr, entry->ai_addrlen, timeoutMs, error)) {
            connected = fd;
        }
        else {
            ::close(fd);
        }
    }
    freeaddrinfo(results);

    if (connected < 0) {
        LOG_DEBUG("cannot connect to {}:{}: {}", host, port, error);
        failed_.store(true);
        return false;
    }
    error.clear();
    return adopt(connected);
}

bool Connection::adopt(int fd) {
    if (fd < 0) {
        return false;
    }
    suppressSigpipe(fd);
    std::lock_guard<std::mutex> lock(fdMutex_);
    if (fd_.load() >= 0) {
        return false;
    }
    fd_.store(fd);
    failed_.store(false);
    return true;
}

std::unique_ptr<Connection> Connection::fromAccepted(int fd) {
    auto connection = std::make_unique<Connection>();
    if (!connection->adopt(fd)) {
        ::close(fd);
        return nullptr;
    }
    connection->startReader();
    return connection;
}

void Connection::startReader() {
    if (fd_.load() < 0 || running_.load()) {
        return;
    }
    running_.store(true);
    reader_ = std::thread([this] { readLoop(); });
}

void Connection::stop() {
    running_.store(false);
    // Shut down before taking fdMutex_: this is what unblocks a send() that
    // holds the lock while stuck on a full buffer, as well as the reader.
    if (const int fd = fd_.load(); fd >= 0) {
        ::shutdown(fd, SHUT_RDWR);
    }
    if (reader_.joinable()) {
        reader_.join();
    }
    std::lock_guard<std::mutex> lock(fdMutex_);
    if (fd_.load() >= 0) {
        ::close(fd_.load());
        fd_.store(-1);
        failed_.store(true);
    }
}

bool Connection::send(const Message& message) {
    const std::string frame = encode(message);
    std::lock_guard<std::mutex> lock(fdMutex_);
    const int fd = fd_.load();

    std::size_t offset = 0;
    while (fd >= 0 && offset < frame.size()) {
        const ssize_t written = ::send(fd, frame.data() + offset, frame.size() - offset,
                                       kSendFlags);
        if (written > 0) {
            offset += static_cast<std::size_t>(written);
        }
        else if (written == 0 || errno != EINTR) {
            break;
        }
    }
    if (offset == frame.size()) {
        return true;
    }
    failed_.store(true);
    return false;
}

bool Connection::poll(Message& out) {
    std::lock_guard<std::mutex> lock(queueMutex_);
    if (queue_.empty()) {
        return false;
    }
    out = std::move(queue_.front());
    queue_.pop_front();
    return true;
}

void Connection::readLoop() {
    // stop() joins this thread before closing the socket, so fd stays valid.
    const int fd = fd_.load();
    std::string buffer;
    char chunk[4096];
    while (running_.load()) {
        const ssize_t bytes = recv(fd, chunk, sizeof(chunk), 0);
        if (bytes < 0 && errno == EINTR) {
            continue;
        }
        if (bytes <= 0) {
            break;
        }
        buffer.append(chunk, static_cast<std::size_t>(bytes));

        Message message;
        DecodeStatus status;
        while ((status = decode(buffer, message)) == DecodeStatus::Ok) {
            std::lock_guard<std::mutex> lock(queueMutex_);
            queue_.push_back(std::move(message));
        }
        if (status == DecodeStatus::Malformed) {
            LOG_DEBUG("closing connection: malformed frame");
            break;
        }
    }
    failed_.store(true);
    running_.store(false);
}

} // namespace chat
