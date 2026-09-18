#include "disquisition/client.h"

#include <chrono>
#include <ctime>
#include <stdexcept>
#include <string>

#include "common/protocol.h"

namespace disquisition
{

Client::Client(std::string address, ConnectionType type)
{
    // Split "relay.mmatt.net:3333" into a host and a port.
    std::size_t colon = address.find_last_of(':');

    if (colon == std::string::npos)
    {
        throw std::invalid_argument("address must include a host and port");
    }

    host = address.substr(0, colon);
    std::string portText = address.substr(colon + 1);

    // Ports are numbers from 1 through 65535. parsePort also rejects text such
    // as "abc" and "3333abc".
    std::uint16_t parsedPort;
    if (host.empty() || !chat::parsePort(portText, parsedPort, false))
    {
        throw std::invalid_argument("address must look like relay.mmatt.net:3333");
    }

    port = parsedPort;
    connectionType = type;
    userColor = 20;
    connected = false;
    nameWasSet = false;
    serviceRunning = false;
    messageHandler = nullptr;
}

Client::~Client()
{
    // disconnect is safe when the client is already disconnected.
    disconnect();
}

void Client::onMessage(MessageHandler handler)
{
    if (connected)
    {
        throw std::runtime_error("set the message handler before connecting");
    }

    if (handler == nullptr)
    {
        throw std::invalid_argument("message handler cannot be empty");
    }

    messageHandler = handler;
}

void Client::connect()
{
    if (connected)
    {
        throw std::runtime_error("client is already connected");
    }

    std::string error;

    if (!serverConnection.connectTo(host, port, error))
    {
        throw std::runtime_error("could not connect: " + error);
    }

    // Direct clients need a port where other clients can reach them. Passing
    // 0 asks the operating system to choose an available port. Relay clients
    // do not listen because the relay accepts peer connections for them.
    if (connectionType == DIRECT && !peerNetwork.start(0, error))
    {
        serverConnection.stop();
        throw std::runtime_error("could not start peer listener: " + error);
    }

    // Connection reads complete protocol messages on its own thread. The
    // service thread below takes those messages and updates the peer list.
    serverConnection.startReader();
    serviceRunning = true;
    serviceThread = std::thread(&Client::serviceConnections, this);
    connected = true;
}

void Client::setName(std::string name)
{
    if (!connected)
    {
        throw std::runtime_error("connect before setting a name");
    }

    if (nameWasSet)
    {
        throw std::runtime_error("name was already set");
    }

    userName = chat::sanitizeName(name);

    if (userName.empty())
    {
        throw std::invalid_argument("name cannot be empty");
    }

    // A direct client tells the server which port its peer listener chose. A
    // relay client sends 0 because the relay owns the public peer port.
    std::string peerPort = "0";
    if (connectionType == DIRECT)
    {
        peerPort = std::to_string(peerNetwork.port());
        peerNetwork.setMyName(userName);
    }

    chat::Message login;
    login.type = chat::MsgType::Login;
    login.fields = {userName, peerPort, ""};

    if (!serverConnection.send(login))
    {
        throw std::runtime_error("could not send login");
    }

    nameWasSet = true;
}

void Client::sendMessage(std::string message)
{
    if (!connected || !nameWasSet)
    {
        throw std::runtime_error("connect and set a name before sending a message");
    }

    std::string cleanMessage = chat::sanitizeBody(message);

    if (cleanMessage.empty())
    {
        throw std::invalid_argument("message cannot be empty");
    }

    std::int64_t timestamp = std::time(nullptr);
    std::string timeText = std::to_string(timestamp);
    std::string colorText = std::to_string(userColor);

    if (connectionType == RELAY)
    {
        // The relay owns this client's peer connections, so it forwards the
        // live message to the peer network.
        chat::Message liveMessage;
        liveMessage.type = chat::MsgType::PeerChat;
        liveMessage.fields = {userName, timeText, cleanMessage, colorText};

        if (!serverConnection.send(liveMessage))
        {
            throw std::runtime_error("could not send message to relay");
        }
    }
    else
    {
        // A direct client already owns its peer connections and writes the
        // live message to each of them.
        peerNetwork.sendChat(timestamp, cleanMessage, colorText);
    }

    // Live delivery and history storage are separate jobs. The live message
    // goes through the peer network. This copy goes to the central server so
    // users who join later can request it as history.
    chat::Message historyCopy;
    historyCopy.type = chat::MsgType::Store;
    historyCopy.fields = {timeText, cleanMessage, colorText};

    if (!serverConnection.send(historyCopy))
    {
        throw std::runtime_error("message was sent, but history could not be saved");
    }
}

void Client::setColor(int color)
{
    // The protocol allows most xterm-256 color numbers. A few numbers belong
    // to system messages, so isValidColor rejects those reserved values too.
    if (!chat::isValidColor(std::to_string(color)))
    {
        throw std::invalid_argument("color must be an available number from 0 to 255");
    }

    userColor = color;
}

void Client::disconnect()
{
    // Stop our service loop before closing the objects it uses.
    serviceRunning = false;
    if (serviceThread.joinable())
    {
        serviceThread.join();
    }

    peerNetwork.stop();
    serverConnection.stop();

    connected = false;
    nameWasSet = false;
}

void Client::serviceConnections()
{
    while (serviceRunning)
    {
        chat::Message message;
        while (serverConnection.poll(message))
        {
            handleServerMessage(message);
        }

        // poll also finishes incoming peer handshakes and removes dead peers.
        // Incoming chat events are discarded until the reading API is added.
        if (connectionType == DIRECT)
        {
            chat::PeerNetwork::Event event;
            while (peerNetwork.poll(event))
            {
                if (event.kind == chat::PeerNetwork::Event::Kind::Chat)
                {
                    deliverMessage(event.name, event.body);
                }
            }
        }

        // Without this short pause the loop would use a full CPU core while it
        // waits for network traffic.
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

void Client::handleServerMessage(const chat::Message& message)
{
    // Relay clients receive live peer messages over their relay connection.
    if (connectionType == RELAY)
    {
        if (message.type == chat::MsgType::PeerChat && message.fields.size() >= 4)
        {
            deliverMessage(message.fields[0], chat::sanitizeBody(message.fields[2]));
        }
        return;
    }

    if (message.type == chat::MsgType::LoginOk && !message.fields.empty())
    {
        // The server may add a suffix when another user already has this name.
        // PeerNetwork must use the final name when it introduces this client.
        peerNetwork.setMyName(message.fields[0]);
    }
    else if ((message.type == chat::MsgType::Peer ||
              message.type == chat::MsgType::PeerJoined) &&
             message.fields.size() >= 4)
    {
        std::uint16_t peerPort;
        if (chat::parsePort(message.fields[2], peerPort, false))
        {
            peerNetwork.addPeer(message.fields[0], message.fields[1], peerPort,
                                message.fields[3] == "1");
        }
    }
    else if (message.type == chat::MsgType::PeerLeft && !message.fields.empty())
    {
        peerNetwork.removePeer(message.fields[0]);
    }
}

void Client::deliverMessage(std::string sender, std::string message)
{
    // Receiving is optional. If the program did not call onMessage, the
    // client still maintains its network connections and discards the text.
    if (messageHandler != nullptr && !message.empty())
    {
        messageHandler(sender, message);
    }
}

} // namespace disquisition
