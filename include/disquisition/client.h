#pragma once

#include <atomic>
#include <string>
#include <thread>

#include "client/connection.h"
#include "client/peer_network.h"

namespace disquisition
{

// Client connects to Disquisition and sends or receives chat messages.
//
// It can connect through a relay or join the peer-to-peer network directly.
// A program that wants to receive messages gives onMessage the name of a
// function to call. The usual order looks like this:
//
//     Client client("relay.mmatt.net:3333");
//     client.onMessage(showMessage);
//     client.connect();
//     client.setName("matt");
//     client.sendMessage("hello");
//     client.disconnect();
class Client
{
public:
    // The enum makes the connection choice readable at the call site.
    enum ConnectionType
    {
        DIRECT,
        RELAY
    };

    // MessageHandler is the kind of function accepted by onMessage. The
    // function receives the sender's name and the text they wrote.
    typedef void (*MessageHandler)(std::string sender, std::string message);

private:
    // The constructor splits the address into these two pieces. For example,
    // "relay.mmatt.net:3333" becomes "relay.mmatt.net" and 3333.
    std::string host;
    int port;

    ConnectionType connectionType;

    // Every chat message includes the sender's name and color.
    std::string userName;
    int userColor;

    // These flags let the class reject methods called in the wrong order and
    // stop the background service loop during disconnect.
    bool connected;
    bool nameWasSet;
    std::atomic<bool> serviceRunning;

    // serverConnection points to the relay in RELAY mode and to the central
    // server in DIRECT mode. peerNetwork is used only in DIRECT mode.
    chat::Connection serverConnection;
    chat::PeerNetwork peerNetwork;
    std::thread serviceThread;
    MessageHandler messageHandler;

    // Direct mode learns about peers from messages sent by the central server.
    // This loop reads those messages and updates peerNetwork.
    void serviceConnections();
    void handleServerMessage(const chat::Message& message);
    void deliverMessage(std::string sender, std::string message);

public:
    // address must contain a host name and port separated by a colon.
    // RELAY is the default so the original one-argument example still works.
    // To connect directly, pass Client::DIRECT as the second argument.
    Client(std::string address, ConnectionType type = RELAY);

    // Closing the socket in the destructor prevents a program from leaving a
    // connection open when a Client object goes out of scope.
    ~Client();

    // Two Client objects must not own the same socket number. Deleting these
    // operations prevents accidental copies such as Client second = first.
    Client(const Client&) = delete;
    Client& operator=(const Client&) = delete;

    // Sets the function that receives incoming messages. Call this before
    // connect. The function runs on the client's background service thread.
    void onMessage(MessageHandler handler);

    // Opens a TCP connection to the address passed to the constructor.
    void connect();

    // Saves the user's name and sends the login message to the relay.
    // A name can be set once per connection.
    void setName(std::string name);

    // Sends one live chat message and one copy for the server's history.
    void sendMessage(std::string message);

    // Changes the xterm-256 color used by future messages.
    void setColor(int color);

    // Closes the socket. It is safe to call this more than once.
    void disconnect();
};

} // namespace disquisition
