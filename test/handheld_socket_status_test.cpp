#include "check.h"
#include "transport.h"

#include <arpa/inet.h>
#include <cerrno>
#include <unistd.h>

#undef getsockopt
#undef getpeername

namespace {
int serviceError = -26;
int pendingPeerChecks = 0;

struct Connection {
    int listener = -1;
    int peer = -1;
    handheld::Transport transport;
    std::vector<chat::Message> received;

    Connection() {
        listener = socket(AF_INET, SOCK_STREAM, 0);
        CHECK(listener >= 0);
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        CHECK(bind(listener, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0);
        CHECK(listen(listener, 1) == 0);
        socklen_t length = sizeof(address);
        CHECK(getsockname(listener, reinterpret_cast<sockaddr*>(&address), &length) == 0);
        CHECK(transport.open("127.0.0.1", ntohs(address.sin_port), 0));
        CHECK(transport.state() == handheld::Transport::State::Connecting);
        peer = accept(listener, nullptr, nullptr);
        CHECK(peer >= 0);
        timeval timeout{1, 0};
        CHECK(setsockopt(peer, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) == 0);
    }

    ~Connection() {
        transport.close();
        if (peer >= 0) close(peer);
        if (listener >= 0) close(listener);
    }

    void pollUntilChanged(std::uint64_t now) {
        for (int i = 0; i < 1000 && transport.state() == handheld::Transport::State::Connecting; ++i) {
            transport.poll(now, received);
            usleep(1000);
        }
    }
};

void staleProgressTest() {
    serviceError = -26;
    pendingPeerChecks = 2;
    Connection connection;
    connection.transport.poll(1, connection.received);
    CHECK(connection.transport.state() == handheld::Transport::State::Connecting);
    CHECK(!connection.transport.queue({chat::MsgType::RelayProbe, {}}));
    connection.pollUntilChanged(2);
    CHECK(connection.transport.state() == handheld::Transport::State::Connected);
    CHECK(connection.transport.error().empty());

    // The stale error must allow actual protocol traffic once a peer exists.
    const auto expected = chat::encode({chat::MsgType::RelayProbe, {}});
    CHECK(connection.transport.queue({chat::MsgType::RelayProbe, {}}));
    connection.transport.poll(3, connection.received);
    std::string bytes(expected.size(), '\0');
    CHECK(recv(connection.peer, bytes.data(), bytes.size(), MSG_WAITALL) ==
          static_cast<ssize_t>(expected.size()));
    CHECK(bytes == expected);
    const auto response = chat::encode({chat::MsgType::RelayReady, {}});
    CHECK(send(connection.peer, response.data(), response.size(), 0) == static_cast<ssize_t>(response.size()));
    for (int i = 0; i < 1000 && connection.received.empty(); ++i) {
        connection.transport.poll(4, connection.received);
        usleep(1000);
    }
    CHECK(connection.received.size() == 1);
    CHECK(connection.received.front().type == chat::MsgType::RelayReady);
}

void timeoutTest(int code) {
    serviceError = code;
    pendingPeerChecks = 100;
    Connection connection;
    connection.transport.poll(9999, connection.received);
    CHECK(connection.transport.state() == handheld::Transport::State::Connecting);
    connection.transport.poll(10000, connection.received);
    CHECK(connection.transport.state() == handheld::Transport::State::Failed);
    CHECK(connection.transport.error() == "Relay connection timed out.");
}

void failureTest(int code) {
    serviceError = code;
    pendingPeerChecks = 0;
    Connection connection;
    connection.pollUntilChanged(1);
    CHECK(connection.transport.state() == handheld::Transport::State::Failed);
    CHECK(connection.transport.error().find("[" + std::to_string(code) + "]") != std::string::npos);
    CHECK(!connection.transport.queue({chat::MsgType::RelayProbe, {}}));
}
} // namespace

int handheld_test_getsockopt(int fd, int level, int option, void* value, socklen_t* length) {
    if (level != SOL_SOCKET || option != SO_ERROR)
        return getsockopt(fd, level, option, value, length);
    CHECK(*length >= sizeof(int));
    *static_cast<int*>(value) = serviceError;
    *length = sizeof(int);
    return 0;
}

int handheld_test_getpeername(int fd, sockaddr* address, socklen_t* length) {
    if (pendingPeerChecks > 0) {
        --pendingPeerChecks;
        errno = ENOTCONN;
        return -1;
    }
    return getpeername(fd, address, length);
}

int main() {
    staleProgressTest();
    timeoutTest(-26);
    timeoutTest(EINPROGRESS);
    timeoutTest(0); // Writable alone does not establish a connected peer.
    failureTest(-14); // Horizon's raw connection-refused code.
    failureTest(-9999); // Unknown service errors remain printable and fatal.
    failureTest(ECONNREFUSED);
    std::puts("handheld_socket_status_test: ok");
}
