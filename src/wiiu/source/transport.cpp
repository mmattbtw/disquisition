#include "transport.h"

#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/tcp.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

namespace handheld {
namespace {
bool wouldBlock() { return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR; }
bool connectPending(int code) {

    return code == EINPROGRESS || code == EALREADY || code == EWOULDBLOCK || code == EAGAIN;
}
std::string systemError(const char* operation, int code) {
    // Raw service error values cannot be passed to strerror safely.
    if (code < 0 || code >= 10000)
        return std::string(operation) + ": socket error [" + std::to_string(code) + "]";
    return std::string(operation) + ": " + std::strerror(code) + " [" + std::to_string(code) + "]";
}
}

void Transport::close() {
    if (socket_ >= 0) ::close(socket_);
    socket_ = -1;
    state_ = State::Closed;
    incoming_.clear();
    outgoing_.clear();
    offset_ = queued_ = 0;
}

void Transport::fail(const std::string& reason) {
    close();
    state_ = State::Failed;
    error_ = reason;
}

bool Transport::open(const std::string& host, std::uint16_t port, std::uint64_t now) {
    close();
    error_.clear();
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    if (inet_pton(AF_INET, host.c_str(), &address.sin_addr) != 1) {
        // wut supplies IPv4 gethostbyname. All DNS use stays on this worker.
        hostent* resolved = gethostbyname(host.c_str());
        if (!resolved || resolved->h_addrtype != AF_INET || resolved->h_length != 4 ||
            !resolved->h_addr_list[0]) {
            fail("DNS failed [" + std::to_string(h_errno) + "]. Check Wi-Fi and relay host.");
            return false;
        }
        std::memcpy(&address.sin_addr, resolved->h_addr_list[0], 4);
    }
    socket_ = socket(AF_INET, SOCK_STREAM, 0);
    if (socket_ < 0) {
        fail(systemError("Socket", errno));
        return false;
    }
    if (socket_ >= FD_SETSIZE) {
        fail("Socket descriptor exceeds select limit.");
        return false;
    }
    if (fcntl(socket_, F_SETFL, O_NONBLOCK) < 0) {
        fail(systemError("Nonblocking socket", errno));
        return false;
    }
    int yes = 1;
    setsockopt(socket_, IPPROTO_TCP, TCP_NODELAY, &yes, sizeof(yes));
#ifdef SO_NOSIGPIPE
    setsockopt(socket_, SOL_SOCKET, SO_NOSIGPIPE, &yes, sizeof(yes));
#endif
    const int result = connect(socket_, reinterpret_cast<sockaddr*>(&address), sizeof(address));
    if (result < 0 && !connectPending(errno)) {
        fail(systemError("TCP connect", errno));
        return false;
    }
    state_ = result == 0 ? State::Connected : State::Connecting;
    deadline_ = now + 10000;
    return true;
}

bool Transport::queue(const chat::Message& message) {
    if (state_ != State::Connected) return false;
    const bool audio = message.type == chat::MsgType::VoiceAudio;
    std::string bytes = chat::encode(message);
    if (audio && queued_ + bytes.size() > 4096) return true;
    if (!audio) {
        // Never remove a partly written frame: that would corrupt the stream.
        for (auto it = outgoing_.begin(); it != outgoing_.end();) {
            if (it->audio && (it != outgoing_.begin() || offset_ == 0)) {
                queued_ -= it->bytes.size();
                it = outgoing_.erase(it);
            } else ++it;
        }
    }
    if (queued_ + bytes.size() > 64 * 1024) {
        fail("Relay is too slow. Reconnecting...");
        return false;
    }
    queued_ += bytes.size();
    outgoing_.push_back({std::move(bytes), audio});
    return true;
}

void Transport::poll(std::uint64_t now, std::vector<chat::Message>& received) {
    if (state_ == State::Connecting) {
        fd_set writes, errors;
        FD_ZERO(&writes); FD_ZERO(&errors);
        FD_SET(socket_, &writes); FD_SET(socket_, &errors);
        timeval timeout{};
        const int ready = select(socket_ + 1, nullptr, &writes, &errors, &timeout);
        if (ready > 0) {
            int error = 0;
            socklen_t length = sizeof(error);
            if (getsockopt(socket_, SOL_SOCKET, SO_ERROR, &error, &length) < 0)
                fail(systemError("Socket status", errno));
            else if (error != 0 && !connectPending(error)
#ifdef __WIIU__
                     // wut translates syscall errno, but SO_ERROR can contain
                     // nsysnet error numbers: EWOULDBLOCK=6, EALREADY=10, EINPROGRESS=22.
                     && error != 6 && error != 10 && error != 22
                     && error != 10006 && error != 10010 && error != 10022
#endif
                     )
                fail(systemError("TCP connect", error));
            else {
                // Verify the peer before declaring a nonblocking connection ready.
                sockaddr_in peer{};
                socklen_t peerLength = sizeof(peer);
                if (getpeername(socket_, reinterpret_cast<sockaddr*>(&peer), &peerLength) == 0)
                    state_ = State::Connected;
                else if (errno != ENOTCONN && !connectPending(errno) && errno != EINTR)
                    fail(systemError("TCP peer", errno));
            }
        } else if (ready < 0 && errno != EINTR) {
            fail(systemError("Socket select", errno));
        }
        if (state_ == State::Connecting && now >= deadline_) {
            fail("Relay connection timed out.");
        }
    }
    if (state_ != State::Connected) return;
    for (int i = 0; i < 16 && !outgoing_.empty(); ++i) {
        const auto& bytes = outgoing_.front().bytes;
        const auto sent = send(socket_, bytes.data() + offset_, bytes.size() - offset_,
#ifdef MSG_NOSIGNAL
                               MSG_NOSIGNAL
#else
                               0
#endif
        );
        if (sent < 0 && wouldBlock()) break;
        if (sent <= 0) { fail(sent < 0 ? systemError("TCP send", errno) : "Relay disconnected."); return; }
        offset_ += static_cast<std::size_t>(sent);
        queued_ -= static_cast<std::size_t>(sent);
        if (offset_ == bytes.size()) { outgoing_.pop_front(); offset_ = 0; }
    }
    for (int i = 0; i < 8; ++i) {
        char buffer[4096];
        const auto got = recv(socket_, buffer, sizeof(buffer), 0);
        if (got < 0 && wouldBlock()) break;
        if (got <= 0) { fail(got < 0 ? systemError("TCP receive", errno) : "Relay disconnected."); return; }
        incoming_.append(buffer, static_cast<std::size_t>(got));
        for (;;) {
            chat::Message message;
            const auto status = chat::decode(incoming_, message);
            if (status == chat::DecodeStatus::Incomplete) break;
            if (status == chat::DecodeStatus::Malformed) {
                fail("Invalid relay frame.");
                return;
            }
            received.push_back(std::move(message));
        }
    }
}

} // namespace handheld
