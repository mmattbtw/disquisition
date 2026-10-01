#pragma once

#include <atomic>
#include <cstdint>
#include <string>
#include <thread>

#include "client/connection.h"
#include "client/peer_network.h"

namespace disquisition {

// Connects to Disquisition to send and receive chat.
//
//     Client client("chat.example.net:9000");
//     client.onMessage(showMessage);
//     client.connect();
//     client.setName("matt");
//     client.sendMessage("hello");
//     client.disconnect();
//
// Methods throw std::invalid_argument for bad values and std::runtime_error
// when called out of order or when the network fails.
class Client {
public:
    enum ConnectionType { DIRECT, RELAY };

    using MessageHandler = void (*)(std::string sender, std::string message);

    // `address` is "host:port" (or "[ipv6]:port") of the server, or of a
    // relay when `type` is RELAY.
    explicit Client(std::string address, ConnectionType type = DIRECT);
    ~Client();

    Client(const Client&) = delete;
    Client& operator=(const Client&) = delete;

    // Call before connect(). The handler runs on a background thread.
    void onMessage(MessageHandler handler);
    void connect();
    // Signs in. Allowed once per connection.
    void setName(std::string name);
    // Delivers the message live to peers or through the relay.
    void sendMessage(std::string message);
    // An xterm-256 color index for future messages.
    void setColor(int color);
    // Safe to call more than once.
    void disconnect();

private:
    void serviceConnections();
    void handleServerMessage(const chat::Message& message);
    void deliverMessage(const std::string& sender, const std::string& message);

    std::string host_;
    std::uint16_t port_ = 0;
    ConnectionType type_ = DIRECT;
    std::string name_;
    int color_ = 20;
    bool connected_ = false;
    bool nameSet_ = false;
    MessageHandler handler_ = nullptr;

    chat::Connection server_; // the relay, in RELAY mode
    chat::PeerNetwork peers_; // used in DIRECT mode only
    std::thread serviceThread_;
    std::atomic<bool> serviceRunning_{false};
};

} // namespace disquisition
