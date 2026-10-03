#pragma once

#include "common/protocol.h"

#include <deque>
#include <string>
#include <vector>

namespace handheld {

// Called only by the worker. DNS can block there, but connect/read/write never
// block the drawing or input thread. poll() also bounds work per service tick.
class Transport {
public:
    enum class State { Closed, Connecting, Connected, Failed };
    ~Transport() { close(); }
    Transport() = default;
    Transport(const Transport&) = delete;
    Transport& operator=(const Transport&) = delete;
    bool open(const std::string& host, std::uint16_t port, std::uint64_t now);
    void close();
    void poll(std::uint64_t now, std::vector<chat::Message>& received);
    // false means reliable traffic overflowed and the connection was closed.
    // Voice frames are disposable and are dropped before they delay text chat.
    bool queue(const chat::Message& message);
    State state() const { return state_; }
    const std::string& error() const { return error_; }
private:
    int socket_ = -1;
    State state_ = State::Closed;
    std::uint64_t deadline_ = 0;
    std::string error_;
    std::string incoming_;
    struct Pending { std::string bytes; bool audio; };
    std::deque<Pending> outgoing_;
    std::size_t offset_ = 0;
    std::size_t queued_ = 0;
    void fail(const std::string& reason);
};

} // namespace handheld
