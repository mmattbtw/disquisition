#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <string>
#include <thread>

#include "check.h"
#include "client/connection.h"
#include "common/net.h"
#include "common/protocol.h"

namespace {

using Clock = std::chrono::steady_clock;

struct Listener {
    int fd = -1;
    std::uint16_t port = 0;

    Listener() {
        std::string error;
        fd = chat::listenTcp(0, error);
        CHECK(fd >= 0);
        port = chat::localPort(fd);
        CHECK(port != 0);
    }
    ~Listener() { ::close(fd); }

    int acceptOne() const {
        const int client = accept(fd, nullptr, nullptr);
        CHECK(client >= 0);
        return client;
    }
};

// This test deliberately leaves SIGPIPE at its default action, which would
// kill the process if a Connection ever let the signal through.
void testWritingToClosedPeerDoesNotRaiseSigpipe() {
    Listener listener;
    chat::Connection connection;
    std::string error;
    CHECK(connection.connectTo("127.0.0.1", listener.port, 3000, error));
    ::close(listener.acceptOne());

    const chat::Message message{chat::MsgType::System, {std::string(1000, 'x')}};
    bool sendFailed = false;
    for (int attempt = 0; attempt < 1000 && !sendFailed; ++attempt) {
        sendFailed = !connection.send(message);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    CHECK(sendFailed);
    CHECK(connection.failed());
}

// stop() has to be able to interrupt a send() that is blocked because the
// other side stopped reading, without deadlocking on the send lock.
void testStopInterruptsBlockedSend() {
    Listener listener;
    chat::Connection connection;
    std::string error;
    CHECK(connection.connectTo("127.0.0.1", listener.port, 3000, error));
    const int silent = listener.acceptOne();

    std::atomic<bool> sendReturned{false};
    std::thread writer([&] {
        const chat::Message big{chat::MsgType::System, {std::string(8000, 'x')}};
        while (connection.send(big)) {
        }
        sendReturned = true;
    });

    // Give the writer time to fill both socket buffers and block.
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    CHECK(!sendReturned);

    const auto started = Clock::now();
    connection.stop();
    writer.join();
    CHECK(sendReturned);
    CHECK(Clock::now() - started < std::chrono::seconds(5));
    CHECK(connection.failed());
    ::close(silent);
}

void testReconnectAfterStop() {
    Listener listener;
    chat::Connection connection;
    std::string error;

    CHECK(connection.connectTo("127.0.0.1", listener.port, 3000, error));
    const int first = listener.acceptOne();
    CHECK(!connection.failed());

    // A second connect while the first socket is open is refused rather
    // than leaking the socket.
    CHECK(!connection.connectTo("127.0.0.1", listener.port, 3000, error));
    CHECK(error == "already connected");

    connection.startReader();
    connection.stop();
    CHECK(connection.failed());
    ::close(first);

    CHECK(connection.connectTo("127.0.0.1", listener.port, 3000, error));
    CHECK(!connection.failed());
    connection.startReader();
    const int second = listener.acceptOne();

    const std::string frame = chat::encode(chat::Message{chat::MsgType::System, {"again"}});
    CHECK(::send(second, frame.data(), frame.size(), 0) == static_cast<ssize_t>(frame.size()));
    chat::Message received;
    const auto deadline = Clock::now() + std::chrono::seconds(5);
    while (!connection.poll(received) && Clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    CHECK(received.type == chat::MsgType::System);
    CHECK(received.fields.at(0) == "again");

    // The peer hanging up marks the connection failed.
    ::close(second);
    const auto hangupDeadline = Clock::now() + std::chrono::seconds(5);
    while (!connection.failed() && Clock::now() < hangupDeadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    CHECK(connection.failed());
}

void testMalformedFrameFailsConnection() {
    Listener listener;
    chat::Connection connection;
    std::string error;
    CHECK(connection.connectTo("127.0.0.1", listener.port, 3000, error));
    connection.startReader();
    const int peer = listener.acceptOne();

    // A zero-length payload is never valid.
    const char zero[4] = {0, 0, 0, 0};
    CHECK(::send(peer, zero, sizeof(zero), 0) == 4);
    const auto deadline = Clock::now() + std::chrono::seconds(5);
    while (!connection.failed() && Clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    CHECK(connection.failed());
    ::close(peer);
}

void testConnectFailureReportsError() {
    // Grab a free port, then close it so nothing is listening there.
    std::uint16_t port = 0;
    {
        Listener listener;
        port = listener.port;
    }
    chat::Connection connection;
    std::string error = "stale";
    CHECK(!connection.connectTo("127.0.0.1", port, 3000, error));
    CHECK(!error.empty() && error != "stale");
    CHECK(connection.failed());
}

} // namespace

int main() {
    testWritingToClosedPeerDoesNotRaiseSigpipe();
    testStopInterruptsBlockedSend();
    testReconnectAfterStop();
    testMalformedFrameFailsConnection();
    testConnectFailureReportsError();
    std::puts("connection_test: ok");
    return 0;
}
