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

#include "client/connection.h"
#include "client/peer_network.h"
#include "common/protocol.h"
#include "relay/options.h"

namespace chat {

// Hosts any number of users on one public port, for clients that cannot
// accept connections themselves. New connections are routed by their first
// frame:
//
//   Login  a client. The relay signs in to the server on its behalf,
//          advertising itself as the user's address, and bridges the
//          client to that user's peer mesh.
//   Hello  a peer dialling one of the hosted users; handed to that user's
//          mesh.
//
// The relay and server do not retain message history.
class Relay {
public:
    explicit Relay(RelayOptions options);
    ~Relay();

    // Runs until SIGINT or SIGTERM. Returns an exit code.
    int run();

private:
    struct User {
        std::string requestedName;
        std::string assignedName;
        std::unique_ptr<Connection> client;
        Connection server;
        PeerNetwork peers;
        bool serverReady = false;   // logged in on the current server connection
        bool clientWaiting = false; // client has not been sent LoginOk yet
        std::chrono::steady_clock::time_point nextServerAttempt;
        std::map<std::string, PeerAddress> roster;
        Message lastUsers{MsgType::Users, {}};
    };

    bool listen(std::string& error);
    void shutdown();
    void acceptLoop();

    void dispatchInbound();
    void route(std::unique_ptr<Connection> connection, const Message& first);
    User* findUser(const std::string& name);
    void attachClient(const std::string& requestedName, std::unique_ptr<Connection> client);
    void dropUser(const std::string& key);

    void serviceUsers();
    void serviceHealth();
    bool serviceUser(User& user);
    void connectServer(User& user);
    void serveClientLogin(User& user);
    void replayRoster(User& user);
    void handleServerMessage(User& user, const Message& message);
    void handleClientMessage(User& user, const Message& message);
    void handlePeerEvent(User& user, const PeerNetwork::Event& event);
    void sendToClient(User& user, const Message& message);

    RelayOptions options_;
    int listenFd_ = -1;
    std::uint16_t boundPort_ = 0;
    std::map<std::string, std::unique_ptr<User>> users_; // keyed by requested name
    Connection healthServer_;
    std::chrono::steady_clock::time_point nextHealthAttempt_{};

    std::mutex mutex_;
    std::deque<std::unique_ptr<Connection>> awaiting_; // accepted, first frame not read yet
    std::atomic<bool> running_{false};
    std::thread acceptThread_;
};

} // namespace chat
