#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <string>
#include <thread>
#include <vector>

#include "check.h"
#include "client/client.h"
#include "common/protocol.h"

namespace {

std::atomic<bool> receivedExpectedMessage{false};

void receiveCallback(std::string sender, std::string message) {
    if (sender == "jesse" && message == "hello matt") {
        receivedExpectedMessage = true;
    }
}

void receiveMessages(int fd, std::vector<chat::Message>& messages, std::size_t expected) {
    std::string buffer;
    while (messages.size() < expected) {
        char chunk[4096];
        const ssize_t size = recv(fd, chunk, sizeof(chunk), 0);
        CHECK(size > 0);
        buffer.append(chunk, static_cast<std::size_t>(size));

        chat::Message message;
        while (chat::decode(buffer, message) == chat::DecodeStatus::Ok) {
            messages.push_back(std::move(message));
        }
    }
}

} // namespace

int main() {
    const int listener = socket(AF_INET, SOCK_STREAM, 0);
    CHECK(listener >= 0);

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;
    CHECK(bind(listener, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0);
    CHECK(listen(listener, 1) == 0);

    socklen_t addressSize = sizeof(address);
    CHECK(getsockname(listener, reinterpret_cast<sockaddr*>(&address), &addressSize) == 0);
    const std::uint16_t port = ntohs(address.sin_port);

    std::vector<chat::Message> messages;
    std::thread server([&] {
        const int client = accept(listener, nullptr, nullptr);
        CHECK(client >= 0);
        receiveMessages(client, messages, 3);

        const std::string incoming = chat::encode(
            chat::Message{chat::MsgType::PeerChat, {"jesse", "123456789", "hello matt", "20"}});
        CHECK(send(client, incoming.data(), incoming.size(), 0) ==
              static_cast<ssize_t>(incoming.size()));
        close(client);
    });

    disquisition::Client client("127.0.0.1:" + std::to_string(port), disquisition::Client::RELAY);
    client.onMessage(receiveCallback);
    client.connect();
    client.setName("matt");
    client.sendMessage("what's up");
    client.setColor(20);

    for (int attempt = 0; attempt < 100 && !receivedExpectedMessage; ++attempt) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    CHECK(receivedExpectedMessage);
    client.disconnect();

    server.join();
    close(listener);

    CHECK(messages.size() == 3);
    CHECK(messages[0].type == chat::MsgType::Login);
    CHECK((messages[0].fields == std::vector<std::string>{"matt", "0", ""}));
    CHECK(messages[1].type == chat::MsgType::PeerChat);
    CHECK(messages[1].fields.size() == 4);
    CHECK(messages[1].fields[0] == "matt");
    CHECK(messages[1].fields[2] == "what's up");
    CHECK(messages[1].fields[3] == "20");
    CHECK(messages[2].type == chat::MsgType::Store);
    CHECK(
        (messages[2].fields == std::vector<std::string>{messages[1].fields[1], "what's up", "20"}));

    // Direct mode sends Login and Store to the central server. PeerChat goes
    // through peer connections instead, so it does not appear here.
    const int directListener = socket(AF_INET, SOCK_STREAM, 0);
    CHECK(directListener >= 0);

    sockaddr_in directAddress{};
    directAddress.sin_family = AF_INET;
    directAddress.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    directAddress.sin_port = 0;
    CHECK(bind(directListener, reinterpret_cast<sockaddr*>(&directAddress),
               sizeof(directAddress)) == 0);
    CHECK(listen(directListener, 1) == 0);

    addressSize = sizeof(directAddress);
    CHECK(getsockname(directListener, reinterpret_cast<sockaddr*>(&directAddress), &addressSize) ==
          0);
    const std::uint16_t directPort = ntohs(directAddress.sin_port);

    std::vector<chat::Message> directMessages;
    std::thread directServer([&] {
        const int directSocket = accept(directListener, nullptr, nullptr);
        CHECK(directSocket >= 0);
        receiveMessages(directSocket, directMessages, 2);
        close(directSocket);
    });

    // A one-argument Client uses direct mode by default.
    disquisition::Client directClient("127.0.0.1:" + std::to_string(directPort));
    directClient.connect();
    directClient.setName("matt");
    directClient.sendMessage("direct hello");
    directClient.disconnect();

    directServer.join();
    close(directListener);

    CHECK(directMessages.size() == 2);
    CHECK(directMessages[0].type == chat::MsgType::Login);
    CHECK(directMessages[0].fields[0] == "matt");
    CHECK(directMessages[0].fields[1] != "0");
    CHECK(directMessages[1].type == chat::MsgType::Store);
    CHECK(directMessages[1].fields[1] == "direct hello");
}
