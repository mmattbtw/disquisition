#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "client/connection.h"
#include "client/peer_network.h"
#include "common/protocol.h"

namespace chat {

struct RelayOptions {
    std::string serverHost = "127.0.0.1";
    std::uint16_t serverPort = 9000;
    // Host other peers should dial to reach this relay. The server falls back
    // to the address it sees us connect from when this is empty, but a relay
    // must be marked advertised so unreachable peers know to call it, so the
    // public hostname is strongly recommended.
    std::string advertiseHost;
    // The one port every hosted user shares: peers dial it to reach any of
    // them and each user's client dials it as its upstream.
    std::uint16_t port = 42069;
    std::size_t maxUsers = 64;
};

// A shared, multi-user mesh relay. One process listens on one port and hosts
// any number of users:
//
//   * a client connects and opens with Login — the relay takes the name from
//     it, signs into the server on that user's behalf (advertising itself as
//     the user's address) and bridges frames between the client and the mesh;
//   * a peer (or another relay) connects and opens with Hello [sender,
//     target] — the relay routes it to the hosted user named by `target`.
//
// The server still owns accounts, discovery and storage. The relay keeps no
// database and stores no messages; it only forwards.
class Relay {
public:
    explicit Relay(RelayOptions options);
    ~Relay();

    // Runs until SIGINT/SIGTERM. Returns a process exit code.
    int run();

private:
    struct PeerInfo {
        std::string host;
        std::uint16_t port = 0;
        bool advertised = false;
    };

    // One hosted user: a private server session, its own peer mesh, and the
    // client socket feeding it. Every user shares the relay's public port, so
    // the mesh dials out per user and inbound links are routed by name.
    struct User {
        std::string requestedName;
        std::string assignedName;
        std::unique_ptr<Connection> client;
        Connection server;
        PeerNetwork peers;
        bool serverReady = false;
        bool clientWaiting = false;
        std::chrono::steady_clock::time_point nextServerAttempt;
        std::map<std::string, PeerInfo> roster;
        Message lastUsers {MsgType::Users, {}};
    };

    bool listen(std::string& error);
    void acceptLoop();
    void dispatchInbound();
    void serviceUsers();

    User* findUser(const std::string& name);
    void attachClient(const std::string& requestedName, std::unique_ptr<Connection> client);
    void dropUser(const std::string& key);

    bool ensureServer(User& user);
    void serveClientLogin(User& user);
    void replayRoster(User& user);
    void handleServerMessage(User& user, const Message& message);
    void handleClientMessage(User& user, const Message& message);
    void handlePeerEvent(User& user, const PeerNetwork::Event& event);
    void sendToClient(User& user, const Message& message);
    void log(const std::string& text) const;

    RelayOptions options_;
    int listenFd_ = -1;
    std::uint16_t boundPort_ = 0;

    std::map<std::string, std::unique_ptr<User>> users_;
    // Accepted sockets whose first frame has not been classified yet.
    std::deque<std::unique_ptr<Connection>> awaiting_;

    mutable std::mutex mutex_;
    std::atomic<bool> running_ {false};
    std::thread acceptThread_;
};

}  // namespace chat
