#include "client/client.h"

#include <chrono>
#include <ctime>
#include <stdexcept>
#include <utility>

#include "common/net.h"
#include "common/protocol.h"
#include "util/log.h"

namespace disquisition {
namespace {

constexpr auto kServicePollInterval = std::chrono::milliseconds(10);

} // namespace

Client::Client(std::string address, ConnectionType type) : type_(type) {
    if (address.find(':') == std::string::npos) {
        throw std::invalid_argument("address must include a host and port");
    }
    if (!chat::parseHostPort(address, host_, port_, 0) || port_ == 0) {
        throw std::invalid_argument("address must look like relay.mmatt.net:3333");
    }
}

Client::~Client() {
    disconnect();
}

void Client::onMessage(MessageHandler handler) {
    if (connected_) {
        throw std::runtime_error("set the message handler before connecting");
    }
    if (handler == nullptr) {
        throw std::invalid_argument("message handler cannot be empty");
    }
    handler_ = handler;
}

void Client::connect() {
    if (connected_) {
        throw std::runtime_error("client is already connected");
    }
    std::string error;
    if (!server_.connectTo(host_, port_, error)) {
        throw std::runtime_error("could not connect: " + error);
    }
    // A relay accepts peers on the client's behalf, so only direct clients listen.
    if (type_ == DIRECT && !peers_.start(0, error)) {
        server_.stop();
        throw std::runtime_error("could not start peer listener: " + error);
    }

    server_.startReader();
    serviceRunning_ = true;
    serviceThread_ = std::thread(&Client::serviceConnections, this);
    connected_ = true;
    LOG_DEBUG("connected to {}:{} ({})", host_, port_, type_ == RELAY ? "relay" : "direct");
}

void Client::setName(std::string name) {
    if (!connected_) {
        throw std::runtime_error("connect before setting a name");
    }
    if (nameSet_) {
        throw std::runtime_error("name was already set");
    }
    name_ = chat::sanitizeName(name);
    if (name_.empty()) {
        throw std::invalid_argument("name cannot be empty");
    }

    std::string peerPort = "0";
    if (type_ == DIRECT) {
        peerPort = std::to_string(peers_.port());
        peers_.setMyName(name_);
    }
    if (!server_.send(chat::Message{chat::MsgType::Login, {name_, peerPort, ""}})) {
        throw std::runtime_error("could not send login");
    }
    nameSet_ = true;
}

void Client::sendMessage(std::string message) {
    if (!connected_ || !nameSet_) {
        throw std::runtime_error("connect and set a name before sending a message");
    }
    const std::string body = chat::sanitizeBody(message);
    if (body.empty()) {
        throw std::invalid_argument("message cannot be empty");
    }

    const std::int64_t timestamp = std::time(nullptr);
    const std::string stamp = std::to_string(timestamp);
    const std::string color = std::to_string(color_);

    if (type_ == RELAY) {
        if (!server_.send(chat::Message{chat::MsgType::PeerChat, {name_, stamp, body, color}})) {
            throw std::runtime_error("could not send message to relay");
        }
    }
    else {
        peers_.sendChat(timestamp, body, color);
    }

    if (!server_.send(chat::Message{chat::MsgType::Store, {stamp, body, color}})) {
        throw std::runtime_error("message was sent, but history could not be saved");
    }
}

void Client::setColor(int color) {
    if (!chat::isValidColor(std::to_string(color))) {
        throw std::invalid_argument("color must be an available number from 0 to 255");
    }
    color_ = color;
}

void Client::disconnect() {
    serviceRunning_ = false;
    if (serviceThread_.joinable()) {
        serviceThread_.join();
    }
    peers_.stop();
    server_.stop();
    connected_ = false;
    nameSet_ = false;
}

void Client::serviceConnections() {
    while (serviceRunning_) {
        chat::Message message;
        while (server_.poll(message)) {
            handleServerMessage(message);
        }
        if (type_ == DIRECT) {
            chat::PeerNetwork::Event event;
            while (peers_.poll(event)) {
                if (event.kind == chat::PeerNetwork::Event::Kind::Chat) {
                    deliverMessage(event.name, event.body);
                }
            }
        }
        std::this_thread::sleep_for(kServicePollInterval);
    }
}

void Client::handleServerMessage(const chat::Message& message) {
    // Through a relay, every sender shares one socket, so trust the frame.
    if (type_ == RELAY) {
        chat::ChatPayload payload;
        if (chat::parsePeerChat(message, payload)) {
            deliverMessage(payload.sender, payload.body);
        }
        return;
    }

    chat::PeerAddress peer;
    if (message.type == chat::MsgType::LoginOk && !message.fields.empty()) {
        LOG_DEBUG("signed in as {}", message.fields[0]);
        // The server may have renamed us to avoid a clash.
        peers_.setMyName(message.fields[0]);
    }
    else if (chat::parsePeerAddress(message, peer)) {
        peers_.addPeer(peer.name, peer.host, peer.port, peer.advertised);
    }
    else if (message.type == chat::MsgType::PeerLeft && !message.fields.empty()) {
        peers_.removePeer(message.fields[0]);
    }
}

void Client::deliverMessage(const std::string& sender, const std::string& message) {
    if (handler_ != nullptr && !message.empty()) {
        handler_(sender, message);
    }
}

} // namespace disquisition
