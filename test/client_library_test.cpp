#include "disquisition/client.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <cassert>
#include <chrono>
#include <string>
#include <thread>
#include <vector>

#include "common/protocol.h"

namespace {

std::atomic<bool> receivedExpectedMessage {false};

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
        assert(size > 0);
        buffer.append(chunk, static_cast<std::size_t>(size));

        chat::Message message;
        while (chat::decode(buffer, message) == chat::DecodeStatus::Ok) {
            messages.push_back(std::move(message));
        }
    }
}

}  // namespace

int main() {
    const int listener = socket(AF_INET, SOCK_STREAM, 0);
    assert(listener >= 0);

    sockaddr_in address {};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;
    assert(bind(listener, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0);
    assert(listen(listener, 1) == 0);

    socklen_t addressSize = sizeof(address);
    assert(getsockname(listener, reinterpret_cast<sockaddr*>(&address), &addressSize) == 0);
    const std::uint16_t port = ntohs(address.sin_port);

    std::vector<chat::Message> messages;
    std::thread server([&] {
        const int client = accept(listener, nullptr, nullptr);
        assert(client >= 0);
        receiveMessages(client, messages, 3);

        const std::string incoming = chat::encode(
            chat::Message {chat::MsgType::PeerChat,
                           {"jesse", "123456789", "hello matt", "20"}});
        assert(send(client, incoming.data(), incoming.size(), 0) ==
               static_cast<ssize_t>(incoming.size()));
        close(client);
    });

    disquisition::Client client("127.0.0.1:" + std::to_string(port));
    client.onMessage(receiveCallback);
    client.connect();
    client.setName("matt");
    client.sendMessage("what's up");
    client.setColor(20);

    for (int attempt = 0; attempt < 100 && !receivedExpectedMessage; ++attempt) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    assert(receivedExpectedMessage);
    client.disconnect();

    server.join();
    close(listener);

    assert(messages.size() == 3);
    assert(messages[0].type == chat::MsgType::Login);
    assert((messages[0].fields == std::vector<std::string> {"matt", "0", ""}));
    assert(messages[1].type == chat::MsgType::PeerChat);
    assert(messages[1].fields.size() == 4);
    assert(messages[1].fields[0] == "matt");
    assert(messages[1].fields[2] == "what's up");
    assert(messages[1].fields[3] == "20");
    assert(messages[2].type == chat::MsgType::Store);
    assert((messages[2].fields == std::vector<std::string> {
        messages[1].fields[1], "what's up", "20"}));

    // Direct mode sends Login and Store to the central server. PeerChat goes
    // through peer connections instead, so it does not appear here.
    const int directListener = socket(AF_INET, SOCK_STREAM, 0);
    assert(directListener >= 0);

    sockaddr_in directAddress {};
    directAddress.sin_family = AF_INET;
    directAddress.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    directAddress.sin_port = 0;
    assert(bind(directListener, reinterpret_cast<sockaddr*>(&directAddress),
                sizeof(directAddress)) == 0);
    assert(listen(directListener, 1) == 0);

    addressSize = sizeof(directAddress);
    assert(getsockname(directListener, reinterpret_cast<sockaddr*>(&directAddress),
                       &addressSize) == 0);
    const std::uint16_t directPort = ntohs(directAddress.sin_port);

    std::vector<chat::Message> directMessages;
    std::thread directServer([&] {
        const int directSocket = accept(directListener, nullptr, nullptr);
        assert(directSocket >= 0);
        receiveMessages(directSocket, directMessages, 2);
        close(directSocket);
    });

    disquisition::Client directClient("127.0.0.1:" + std::to_string(directPort),
                                      disquisition::Client::DIRECT);
    directClient.connect();
    directClient.setName("matt");
    directClient.sendMessage("direct hello");
    directClient.disconnect();

    directServer.join();
    close(directListener);

    assert(directMessages.size() == 2);
    assert(directMessages[0].type == chat::MsgType::Login);
    assert(directMessages[0].fields[0] == "matt");
    assert(directMessages[0].fields[1] != "0");
    assert(directMessages[1].type == chat::MsgType::Store);
    assert(directMessages[1].fields[1] == "direct hello");
}
