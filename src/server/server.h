#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "common/protocol.h"
#include "server/database.h"

namespace chat {

struct ServerOptions {
    std::uint16_t port = 9000;
    std::string databasePath = "chat.db";
    std::size_t historyLimit = 50;
    std::size_t maxClients = 128;
};

// Single threaded server built on poll(2). One event loop owns every socket and
// the database, so there is no locking anywhere and no partial-write races.
class Server {
public:
    Server(ServerOptions options, Database& database);

    // Binds and starts listening. Throws std::runtime_error on failure.
    void listen();

    // Serves clients until a SIGINT/SIGTERM arrives.
    void run();

    std::uint16_t port() const { return boundPort_; }

private:
    struct Connection {
        int fd = -1;
        std::string in;
        std::string out;
        std::string name;
        // Address the client connected from, announced to other peers so they
        // can open direct TCP connections to it. If the client supplied its
        // own advertised host (e.g. a public IP) that takes precedence.
        std::string host;
        std::string advertisedHost;
        std::uint16_t peerPort = 0;
        bool authenticated = false;
        bool closing = false;
    };

    void acceptClients();
    bool readFrom(Connection& connection);
    bool writeTo(Connection& connection);
    bool handleFrames(Connection& connection);

    void handleLogin(Connection& connection, const Message& message);
    void handleStore(Connection& connection, const Message& message);
    void handleSetColor(Connection& connection, const Message& message);
    void handleFetchHistory(Connection& connection);
    void sendHistory(Connection& connection);
    void reject(Connection& connection, const std::string& reason);

    void send(Connection& connection, const Message& message);
    void broadcast(const Message& message, const Connection* except = nullptr);
    void broadcastUsers();

    std::string uniqueName(const std::string& requested) const;
    const std::string& hostFor(const Connection& connection) const;
    void dropConnection(std::size_t index);
    void log(const std::string& text) const;

    ServerOptions options_;
    Database& database_;
    int listenFd_ = -1;
    std::uint16_t boundPort_ = 0;
    std::vector<Connection> connections_;
};

}  // namespace chat
