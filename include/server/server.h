#pragma once

#include <poll.h>

#include <cstdint>
#include <string>
#include <vector>

#include "common/protocol.h"
#include "server/options.h"

namespace chat {

// Serves until SIGINT or SIGTERM. Returns an exit code.
int runServer(const ServerOptions& options);

// The sign-in and discovery server. A single poll() loop owns every
// socket, so nothing needs locking.
class Server {
public:
    explicit Server(ServerOptions options);
    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;
    ~Server();

    // Both throw std::runtime_error on failure.
    void listen();
    void run();

    std::uint16_t port() const { return boundPort_; }

private:
    struct Client {
        int fd = -1;
        std::string in;
        std::string out;
        std::string name;
        std::string host;           // the address the client connected from
        std::string advertisedHost; // overrides `host` when announcing the client
        std::uint16_t peerPort = 0;
        bool authenticated = false;
        bool closing = false; // drop once `out` is flushed
    };

    std::vector<pollfd> pollSet() const;
    void acceptClients();
    bool service(Client& client, short revents);
    bool readFrom(Client& client);
    bool writeTo(Client& client);
    bool handleFrames(Client& client);

    void handleLogin(Client& client, const Message& message);
    void handleStore(Client& client, const Message& message);
    void handleFetchHistory(Client& client);
    void reject(Client& client, const std::string& reason);

    void send(Client& client, const Message& message);
    void broadcast(const Message& message, const Client* except = nullptr);
    void broadcastUsers();

    std::string uniqueName(const std::string& requested) const;
    PeerAddress addressOf(const Client& client) const;
    void dropClient(std::size_t index);
    static std::string describe(const Client& client); // for log lines

    ServerOptions options_;
    int listenFd_ = -1;
    std::uint16_t boundPort_ = 0;
    std::vector<Client> clients_;
};

} // namespace chat
