#pragma once

#include <atomic>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "common/protocol.h"

namespace chat {

// A TCP connection that decodes incoming frames on a background thread.
//
// send(), poll() and failed() are safe from any thread. connectTo(), adopt(),
// startReader() and stop() must not race each other, but may run while
// another thread is sending.
class Connection {
public:
    Connection() = default;
    Connection(const Connection&) = delete;
    Connection& operator=(const Connection&) = delete;
    ~Connection();

    // A negative timeout waits as long as the operating system does. Both
    // overloads, and adopt(), fail if a socket is already open.
    bool connectTo(const std::string& host, std::uint16_t port, std::string& error);
    bool connectTo(const std::string& host, std::uint16_t port, int timeoutMs, std::string& error);
    // Takes ownership of an already connected socket, e.g. from accept().
    bool adopt(int fd);
    // Wraps a socket from accept() and starts reading. Closes `fd` on failure.
    static std::unique_ptr<Connection> fromAccepted(int fd);

    void startReader();
    void stop();

    bool send(const Message& message);
    bool poll(Message& out);

    // True after an error, a remote close or stop(), until the next connect.
    bool failed() const { return failed_.load(); }

private:
    void readLoop();

    // Held while fd_ changes and for each whole send(), so a frame is never
    // cut short or written to a closed and reused descriptor.
    std::mutex fdMutex_;
    std::atomic<int> fd_{-1};
    std::thread reader_;
    std::atomic<bool> running_{false};
    std::atomic<bool> failed_{false};

    std::mutex queueMutex_;
    std::deque<Message> queue_;
};

} // namespace chat
