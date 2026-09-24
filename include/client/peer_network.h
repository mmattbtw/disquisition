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

// Full mesh of direct TCP links to every other online user.
//
// The server announces who is online; this class keeps exactly one link per
// pair of peers. When both peers are equally reachable, the one whose name
// sorts first dials; otherwise the unreachable one dials out. The dialler
// opens with Hello and the listener replies HelloOk.
class PeerNetwork {
public:
    struct Event {
        enum class Kind { Chat, Join, Leave, Note };
        Kind kind = Kind::Note;
        std::int64_t timestamp = 0;
        std::string name;
        std::string body;
        std::string color;
    };

    PeerNetwork() = default;
    PeerNetwork(const PeerNetwork&) = delete;
    PeerNetwork& operator=(const PeerNetwork&) = delete;
    ~PeerNetwork();

    // Listens on `port` (0 picks one) and starts dialling.
    bool start(std::uint16_t port, std::string& error);
    // Dials without listening. The relay uses this: its shared listener hands
    // inbound peers over with adoptInbound().
    bool startDetached(std::string& error);
    void stop();

    std::uint16_t port() const { return listenPort_; }

    // The server-assigned name. Call it before adding peers: it decides who
    // dials whom.
    void setMyName(const std::string& name);
    // Whether this user advertised a publicly reachable address.
    void setMyAdvertised(bool advertised);
    // Tracks the roster but never dials. Used while a relay owns the links.
    void setPassive(bool passive);

    void addPeer(const std::string& name, const std::string& host, std::uint16_t port,
                 bool advertised);
    void removePeer(const std::string& name);
    // Takes a connection whose Hello was already read by a shared listener.
    void adoptInbound(const std::string& name, std::unique_ptr<Connection> connection);

    bool connectedTo(const std::string& name) const;
    bool peerAddress(const std::string& name, std::string& host, std::uint16_t& port) const;
    // True once no dial or handshake is in progress.
    bool settled() const;

    void sendChat(std::int64_t timestamp, const std::string& body, const std::string& color);
    // Returns the next event. Also completes inbound handshakes and notices
    // dropped links.
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

    struct DialTarget {
        std::string name;
        std::string host;
        std::uint16_t port = 0;
        std::string myName;
    };

    void acceptLoop();
    void dialLoop();
    bool nextDialTarget(DialTarget& target);
    std::unique_ptr<Connection> dialWithRetries(const DialTarget& target);
    void finishDial(const std::string& name, std::unique_ptr<Connection> link);

    // The helpers below expect mutex_ to be held.
    bool shouldDial(const Peer& peer) const;
    bool isAwaitingDial(const std::string& name) const;
    void dialEligiblePeers();
    void enqueueDial(const std::string& name);
    bool attachInbound(const std::string& remote, std::unique_ptr<Connection> connection);
    void drainPending();
    void drainPeers();
    void pushEvent(Event::Kind kind, const std::string& name, const std::string& body = "",
                   std::int64_t timestamp = 0, const std::string& color = "");

    int listenFd_ = -1;
    std::uint16_t listenPort_ = 0;

    mutable std::mutex mutex_;
    std::string myName_;
    bool myAdvertised_ = false;
    bool passive_ = false;
    std::map<std::string, Peer> peers_;
    std::vector<std::unique_ptr<Connection>> pending_; // inbound, awaiting Hello
    std::deque<std::string> dialQueue_;
    std::deque<Event> events_;

    std::condition_variable dialCv_;
    std::thread acceptThread_;
    std::thread dialThread_;
    std::atomic<bool> running_{false};
};

} // namespace chat
