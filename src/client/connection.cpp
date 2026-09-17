#include "client/connection.h"

#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>

namespace chat {

namespace {

bool setBlocking(int fd, bool blocking) {
    const int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) {
        return false;
    }
    const int updated = blocking ? (flags & ~O_NONBLOCK) : (flags | O_NONBLOCK);
    return fcntl(fd, F_SETFL, updated) == 0;
}

// Connects one already-created socket, honouring timeoutMs (< 0 blocks).
bool finishConnect(int fd, const sockaddr* address, socklen_t length, int timeoutMs,
                   std::string& error) {
    if (timeoutMs < 0) {
        if (::connect(fd, address, length) == 0) {
            error.clear();
            return true;
        }
        error = std::strerror(errno);
        return false;
    }

    if (!setBlocking(fd, false)) {
        error = std::strerror(errno);
        return false;
    }
    if (::connect(fd, address, length) == 0) {
        error.clear();
        if (!setBlocking(fd, true)) {
            error = std::strerror(errno);
            return false;
        }
        return true;
    }
    if (errno != EINPROGRESS) {
        error = std::strerror(errno);
        return false;
    }

    pollfd watched {fd, POLLOUT, 0};
    const int ready = ::poll(&watched, 1, timeoutMs);
    if (ready < 0) {
        error = std::strerror(errno);
        return false;
    }
    if (ready == 0) {
        error = "connection timed out";
        return false;
    }
    int refused = 0;
    socklen_t size = sizeof(refused);
    if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &refused, &size) != 0) {
        error = std::strerror(errno);
        return false;
    }
    if (refused != 0) {
        error = std::strerror(refused);
        return false;
    }
    if (!setBlocking(fd, true)) {
        error = std::strerror(errno);
        return false;
    }
    error.clear();
    return true;
}

}  // namespace

Connection::~Connection() {
    stop();
}

bool Connection::connectTo(const std::string& host, std::uint16_t port, std::string& error) {
    return connectTo(host, port, -1, error);
}

bool Connection::connectTo(const std::string& host, std::uint16_t port, int timeoutMs,
                           std::string& error) {
    addrinfo hints {};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    const std::string service = std::to_string(port);
    addrinfo* results = nullptr;
    const int rc = getaddrinfo(host.c_str(), service.c_str(), &hints, &results);
    if (rc != 0) {
        error = gai_strerror(rc);
        failed_.store(true);
        return false;
    }

    for (addrinfo* entry = results; entry != nullptr; entry = entry->ai_next) {
        const int fd = socket(entry->ai_family, entry->ai_socktype, entry->ai_protocol);
        if (fd < 0) {
            error = std::strerror(errno);
            continue;
        }
        if (finishConnect(fd, entry->ai_addr, entry->ai_addrlen, timeoutMs, error)) {
            fd_ = fd;
            break;
        }
        ::close(fd);
    }
    freeaddrinfo(results);

    if (fd_ < 0 && error.empty()) {
        error = "could not connect";
    }
    failed_.store(fd_ < 0);
    return fd_ >= 0;
}

bool Connection::adopt(int fd) {
    if (fd < 0 || fd_ >= 0) {
        return false;
    }
    fd_ = fd;
    return true;
}

void Connection::startReader() {
    if (fd_ < 0 || running_.load()) {
        return;
    }
    running_.store(true);
    reader_ = std::thread([this] { readLoop(); });
}

void Connection::stop() {
    if (!running_.exchange(false) && fd_ < 0 && !reader_.joinable()) {
        return;
    }
    if (fd_ >= 0) {
        // Unblocks the reader thread's recv() so it can exit promptly.
        ::shutdown(fd_, SHUT_RDWR);
    }
    if (reader_.joinable()) {
        reader_.join();
    }
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
}

bool Connection::send(const Message& message) {
    const std::string frame = encode(message);
    std::lock_guard<std::mutex> lock(sendMutex_);

    std::size_t offset = 0;
    while (offset < frame.size()) {
        const ssize_t written = ::send(fd_, frame.data() + offset, frame.size() - offset, 0);
        if (written > 0) {
            offset += static_cast<std::size_t>(written);
            continue;
        }
        if (written < 0 && errno == EINTR) {
            continue;
        }
        failed_.store(true);
        return false;
    }
    return true;
}

bool Connection::poll(Message& out) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (queue_.empty()) {
        return false;
    }
    out = std::move(queue_.front());
    queue_.pop_front();
    return true;
}

void Connection::readLoop() {
    std::string buffer;
    while (running_.load()) {
        char chunk[4096];
        const ssize_t bytes = recv(fd_, chunk, sizeof(chunk), 0);
        if (bytes > 0) {
            buffer.append(chunk, static_cast<std::size_t>(bytes));

            Message message;
            for (;;) {
                const DecodeStatus status = decode(buffer, message);
                if (status == DecodeStatus::Ok) {
                    std::lock_guard<std::mutex> lock(mutex_);
                    queue_.push_back(std::move(message));
                    continue;
                }
                if (status == DecodeStatus::Malformed) {
                    failed_.store(true);
                    running_.store(false);
                }
                break;
            }
            continue;
        }
        if (bytes == 0) {
            failed_.store(true);
            running_.store(false);
            return;
        }
        if (errno == EINTR) {
            continue;
        }
        failed_.store(true);
        running_.store(false);
        return;
    }
}

}  // namespace chat
