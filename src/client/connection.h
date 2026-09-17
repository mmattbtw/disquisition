#pragma once

#include <atomic>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>

#include "common/protocol.h"

namespace chat {

// A connected socket with a background reader thread. The main (UI) thread
// sends and drains decoded messages, the reader thread only ever receives.
class Connection {
public:
    Connection() = default;
    Connection(const Connection&) = delete;
    Connection& operator=(const Connection&) = delete;
    ~Connection();

    bool connectTo(const std::string& host, std::uint16_t port, std::string& error);
    // Same, but each address attempt gives up after timeoutMs instead of
    // blocking. Negative means wait however long the kernel takes.
    bool connectTo(const std::string& host, std::uint16_t port, int timeoutMs, std::string& error);
    void startReader();
    void stop();

    // Takes ownership of an already connected socket, such as one returned by
    // accept(2) on a peer listener, instead of dialling out with connectTo.
    bool adopt(int fd);

    bool send(const Message& message);

    // Pops one decoded message if the reader thread has delivered one.
    bool poll(Message& out);

    bool failed() const { return failed_.load(); }

private:
    void readLoop();

    int fd_ = -1;
    std::thread reader_;
    std::atomic<bool> running_ {false};
    std::atomic<bool> failed_ {false};
    std::mutex mutex_;
    std::mutex sendMutex_;
    std::deque<Message> queue_;
};

}  // namespace chat
