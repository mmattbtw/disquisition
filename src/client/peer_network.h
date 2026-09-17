#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "client/connection.h"

namespace chat {

// Full-mesh peer-to-peer layer. Sign-in, discovery and storage still go
// through the server, but chat frames travel directly between peers over raw
// TCP connections. To keep exactly one link per pair without negotiating,
// the peer whose name sorts first is the one that dials; it announces itself
// with a Hello frame and the listener learns the remote name from it.
class PeerNetwork {
public:
    struct Event {
        enum class Kind { Chat, Join, Leave, Note };
        Kind kind = Kind::Note;
        std::int64_t timestamp = 0;
        std::string name;
        std::string body;
    };

    PeerNetwork() = default;
    PeerNetwork(const PeerNetwork&) = delete;
    PeerNetwork& operator=(const PeerNetwork&) = delete;
    ~PeerNetwork();

    // Binds the listener (`preferredPort` 0 picks a free port) and starts the
    // accept and dial threads. False with `error` set on failure.
    bool start(std::uint16_t preferredPort, std::string& error);
    void stop();

    std::uint16_t port() const { return listenPort_; }

    // Our server-assigned name. Drives the dial rule, so call it on LoginOk
    // before feeding the roster in.
    void setMyName(const std::string& name);

    // Whether we advertise a publicly reachable host (the user passed
    // --advertise). Reachability decides who dials whom across the internet.
    void setMyAdvertised(bool advertised);

    // Records (or refreshes) a roster entry and dials it when the rule says
    // the call is ours to make.
    void addPeer(const std::string& name, const std::string& host, std::uint16_t port,
                 bool advertised);
    void removePeer(const std::string& name);

    bool connectedTo(const std::string& name) const;
    std::size_t connectedCount() const;

    // Fills `host` and `port` with the address peers are told to dial for
    // `name`, if the roster knows about that peer.
    bool peerAddress(const std::string& name, std::string& host, std::uint16_t& port) const;

    // True when nothing is dialling and every inbound connection has been
    // identified: the mesh is as complete as it is going to get, so history
    // fetched now cannot miss anything a reachable peer already stored.
    bool settled() const;

    // Delivers one frame to every connected peer. A peer that died mid-send
    // surfaces as a Leave event on the next poll().
    void sendChat(std::int64_t timestamp, const std::string& body);

    // Pops one event if there is one. New inbound connections are identified
    // and dead peers reaped along the way.
    bool poll(Event& out);

private:
    struct Peer {
        std::string name;
        std::string host;
        std::uint16_t port = 0;
        bool advertised = false;
        bool dialing = false;
        std::unique_ptr<Connection> connection;
    };

    void acceptLoop();
    void dialLoop();
    void drainPending();
    void drainPeers();
    void enqueueDial(const std::string& name);
    void pushEvent(Event::Kind kind, const std::string& name, const std::string& body,
                   std::int64_t timestamp);
    bool shouldDial(const Peer& peer) const;
    void scanPeersForDialsLocked();

    int listenFd_ = -1;
    std::uint16_t listenPort_ = 0;

    std::string myName_;
    bool myAdvertised_ = false;
    std::map<std::string, Peer> peers_;
    std::vector<std::unique_ptr<Connection>> pending_;
    std::deque<std::string> dialQueue_;
    std::deque<Event> events_;

    mutable std::mutex mutex_;
    std::condition_variable dialCv_;
    std::thread acceptThread_;
    std::thread dialThread_;
    std::atomic<bool> running_ {false};
};

}  // namespace chat
