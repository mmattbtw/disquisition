// Black-box test of the whole system: it launches the real server and relay
// executables, connects library clients to them, and checks that chat
// travels peer to peer and through the relay without server history.
//
// usage: end_to_end_test <path to server> <path to relay> <scratch directory>

#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "check.h"
#include "client/client.h"
#include "client/connection.h"
#include "common/protocol.h"

extern char** environ;

namespace {

using Clock = std::chrono::steady_clock;

// A child process whose stdout is piped back to us. A background thread keeps
// the pipe drained (echoing it to stderr to help debug failures) so a chatty
// child can never block on a full pipe.
struct Child {
    pid_t pid = -1;
    std::uint16_t port = 0;
    std::thread drain;
};

Child gServer;
Child gRelay;

void killChildren() {
    for (Child* child : {&gServer, &gRelay}) {
        if (child->pid > 0) {
            kill(child->pid, SIGKILL);
            waitpid(child->pid, nullptr, 0);
            child->pid = -1;
        }
    }
}

// Starts `argv` and waits for it to print "listening on port N".
void spawnListening(Child& child, const std::vector<std::string>& args) {
    int pipeFds[2];
    CHECK(pipe(pipeFds) == 0);

    posix_spawn_file_actions_t actions;
    CHECK(posix_spawn_file_actions_init(&actions) == 0);
    CHECK(posix_spawn_file_actions_adddup2(&actions, pipeFds[1], STDOUT_FILENO) == 0);
    CHECK(posix_spawn_file_actions_addclose(&actions, pipeFds[0]) == 0);
    CHECK(posix_spawn_file_actions_addclose(&actions, pipeFds[1]) == 0);

    std::vector<char*> argv;
    for (const std::string& arg : args) {
        argv.push_back(const_cast<char*>(arg.c_str()));
    }
    argv.push_back(nullptr);
    const int spawned = posix_spawn(&child.pid, argv[0], &actions, nullptr, argv.data(), environ);
    posix_spawn_file_actions_destroy(&actions);
    close(pipeFds[1]);
    if (spawned != 0) {
        std::fprintf(stderr, "cannot start %s: %s\n", argv[0], std::strerror(spawned));
        CHECK(spawned == 0);
    }

    const int readFd = pipeFds[0];
    std::string line;
    const std::string marker = "listening on port ";
    for (;;) {
        char byte = 0;
        const ssize_t got = read(readFd, &byte, 1);
        if (got != 1) {
            // The child closed its stdout without announcing a port: it
            // most likely exited. Its own stderr (inherited) says why.
            int status = 0;
            const pid_t exited = waitpid(child.pid, &status, WNOHANG);
            std::fprintf(stderr, "%s stopped before it started listening (%s, exit status %d)\n",
                         argv[0], exited == child.pid ? "exited" : "still running",
                         exited == child.pid && WIFEXITED(status) ? WEXITSTATUS(status) : -1);
            if (exited == child.pid) {
                child.pid = -1;
            }
            CHECK(got == 1);
        }
        if (byte != '\n') {
            line.push_back(byte);
            continue;
        }
        std::fprintf(stderr, "  | %s\n", line.c_str());
        const auto at = line.find(marker);
        if (at != std::string::npos) {
            std::string digits = line.substr(at + marker.size());
            digits = digits.substr(0, digits.find_first_not_of("0123456789"));
            CHECK(chat::parsePort(digits, child.port, false));
            break;
        }
        line.clear();
    }

    child.drain = std::thread([readFd] {
        std::string pending;
        char chunk[512];
        ssize_t got = 0;
        while ((got = read(readFd, chunk, sizeof(chunk))) > 0) {
            pending.append(chunk, static_cast<std::size_t>(got));
            std::size_t newline = 0;
            while ((newline = pending.find('\n')) != std::string::npos) {
                std::fprintf(stderr, "  | %s\n", pending.substr(0, newline).c_str());
                pending.erase(0, newline + 1);
            }
        }
        close(readFd);
    });
}

// Asks the child to shut down the way an operator would and expects a clean
// exit.
void stopChild(Child& child) {
    CHECK(kill(child.pid, SIGTERM) == 0);
    int status = 0;
    CHECK(waitpid(child.pid, &status, 0) == child.pid);
    child.pid = -1;
    child.drain.join();
    CHECK(WIFEXITED(status));
    CHECK(WEXITSTATUS(status) == 0);
}

// The library takes a plain function pointer, so each client gets its own
// handler writing into its own inbox.
struct Inbox {
    std::mutex mutex;
    std::vector<std::pair<std::string, std::string>> messages;

    bool contains(const std::string& sender, const std::string& body) {
        std::lock_guard<std::mutex> lock(mutex);
        return std::find(messages.begin(), messages.end(), std::make_pair(sender, body)) !=
               messages.end();
    }
};

Inbox gAliceInbox;
Inbox gBobInbox;
Inbox gCarolInbox;

void aliceHandler(std::string sender, std::string body) {
    std::lock_guard<std::mutex> lock(gAliceInbox.mutex);
    gAliceInbox.messages.emplace_back(std::move(sender), std::move(body));
}
void bobHandler(std::string sender, std::string body) {
    std::lock_guard<std::mutex> lock(gBobInbox.mutex);
    gBobInbox.messages.emplace_back(std::move(sender), std::move(body));
}
void carolHandler(std::string sender, std::string body) {
    std::lock_guard<std::mutex> lock(gCarolInbox.mutex);
    gCarolInbox.messages.emplace_back(std::move(sender), std::move(body));
}

bool waitFor(const std::function<bool()>& condition, int timeoutMs = 10000) {
    const auto deadline = Clock::now() + std::chrono::milliseconds(timeoutMs);
    while (Clock::now() < deadline) {
        if (condition()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return condition();
}

// Live chat is not stored or retried, so a message sent before the mesh link
// is up is simply lost. Keep sending until the receiver has seen it.
void deliverEventually(disquisition::Client& from, const std::string& fromName, Inbox& to,
                       const std::string& body) {
    const auto deadline = Clock::now() + std::chrono::seconds(15);
    while (Clock::now() < deadline) {
        from.sendMessage(body);
        if (waitFor([&] { return to.contains(fromName, body); }, 250)) {
            return;
        }
    }
    std::fprintf(stderr, "%s never received \"%s\" from %s\n", "receiver", body.c_str(),
                 fromName.c_str());
    CHECK(false);
}

// A bare protocol session against the server, for checking what the library
// clients cannot observe.
struct RawSession {
    chat::Connection connection;

    explicit RawSession(std::uint16_t port) {
        std::string error;
        CHECK(connection.connectTo("127.0.0.1", port, 3000, error));
        connection.startReader();
    }

    chat::Message next(chat::MsgType wanted) {
        chat::Message message;
        const bool found = waitFor([&] {
            while (connection.poll(message)) {
                if (message.type == wanted) {
                    return true;
                }
            }
            return false;
        });
        CHECK(found);
        return message;
    }
};

void testServerRejectsUnexpectedFrames(std::uint16_t serverPort) {
    RawSession session(serverPort);
    CHECK(session.connection.send(chat::Message{chat::MsgType::Store, {"1", "early", "pink"}}));
    const chat::Message error = session.next(chat::MsgType::Error);
    CHECK((error.fields == std::vector<std::string>{"sign in first"}));
    CHECK(waitFor([&] { return session.connection.failed(); }));
}

void testVoiceAndRelayHealth(std::uint16_t serverPort, std::uint16_t relayPort) {
    RawSession probe(relayPort);
    CHECK(probe.connection.send(chat::Message{chat::MsgType::RelayProbe, {}}));
    CHECK(probe.next(chat::MsgType::RelayReady).fields.empty());

    RawSession direct(serverPort);
    CHECK(direct.connection.send(
        chat::Message{chat::MsgType::Login, {"voice_direct", "1", ""}}));
    const chat::Message directOk = direct.next(chat::MsgType::LoginOk);
    CHECK(directOk.fields.at(0) == "voice_direct");
    CHECK(chat::hasFeature(directOk.fields.at(2), chat::kSpeakingFeature));

    RawSession relayed(relayPort);
    CHECK(relayed.connection.send(chat::Message{chat::MsgType::Login, {"voice_relay", "0", ""}}));
    const chat::Message relayedOk = relayed.next(chat::MsgType::LoginOk);
    CHECK(relayedOk.fields.at(0) == "voice_relay");
    CHECK(chat::hasFeature(relayedOk.fields.at(2), chat::kSpeakingFeature));

    CHECK(direct.connection.send(chat::Message{chat::MsgType::VoicePort, {"5060"}}));
    CHECK(relayed.next(chat::MsgType::VoicePort).fields ==
          (std::vector<std::string>{"voice_direct", "5060"}));
    CHECK(relayed.connection.send(chat::Message{chat::MsgType::VoicePort, {"65535"}}));
    CHECK(direct.next(chat::MsgType::VoicePort).fields ==
          (std::vector<std::string>{"voice_relay", "65535"}));

    CHECK(relayed.connection.send(
        chat::Message{chat::MsgType::VoiceState, {"voice_relay", "1", "0"}}));
    CHECK(direct.next(chat::MsgType::VoiceState).fields ==
          (std::vector<std::string>{"voice_relay", "1", "0"}));

    CHECK(relayed.connection.send(chat::Message{chat::MsgType::Speaking, {"1"}}));
    CHECK(direct.next(chat::MsgType::Speaking).fields ==
          (std::vector<std::string>{"voice_relay", "1"}));
    // A malformed update is dropped without cutting the sender off.
    CHECK(direct.connection.send(chat::Message{chat::MsgType::Speaking, {"2"}}));
    CHECK(direct.connection.send(chat::Message{chat::MsgType::Speaking, {"0"}}));
    CHECK(relayed.next(chat::MsgType::Speaking).fields ==
          (std::vector<std::string>{"voice_direct", "0"}));

    const std::string pcm(640, '\0');
    CHECK(relayed.connection.send(chat::Message{chat::MsgType::VoiceAudio, {pcm}}));
    CHECK(direct.next(chat::MsgType::VoiceAudio).fields ==
          (std::vector<std::string>{"voice_relay", pcm}));
    CHECK(direct.connection.send(chat::Message{chat::MsgType::VoiceAudio, {pcm}}));
    CHECK(relayed.next(chat::MsgType::VoiceAudio).fields ==
          (std::vector<std::string>{"voice_direct", pcm}));
}

void signIn(RawSession& session, const std::string& name) {
    CHECK(session.connection.send(chat::Message{chat::MsgType::Login, {name, "1", ""}}));
    CHECK(session.next(chat::MsgType::LoginOk).fields.at(0) == name);
}

void testPersonalSprites(std::uint16_t serverPort, std::uint16_t relayPort) {
    using chat::MsgType;
    using chat::Message;
    RawSession alice(serverPort);
    signIn(alice, "sprite_alice");
    RawSession bob(relayPort);
    signIn(bob, "sprite_bob");
    const std::string aliceSprite(chat::kImageChunkSize + 42, '\0');
    const std::string bobSprite(chat::kImageChunkSize * 2 + 1, 'b');
    auto upload = [](RawSession& session, const std::string& data) {
        for (const auto& frame : chat::spriteMessages(data)) CHECK(session.connection.send(frame));
    };
    auto receive = [](RawSession& session, const std::string& name, const std::string& data) {
        chat::SpriteTransfer transfer;
        for (;;) {
            const auto frame = session.next(MsgType::PenguinSprite);
            CHECK(!frame.fields.empty() && frame.fields[0] == name);
            CHECK(transfer.accept(frame, true));
            if (transfer.complete()) break;
        }
        CHECK(transfer.data() == data);
    };
    // HistoryEnd is a barrier after login and its sprite replay. Collect every
    // sprite before it, including any reset, without relying on arbitrary sleeps.
    auto snapshot = [](RawSession& session) {
        CHECK(session.connection.send(Message{MsgType::FetchHistory, {}}));
        std::map<std::string, chat::SpriteTransfer> transfers;
        std::map<std::string, std::string> sprites;
        bool finished = false;
        CHECK(waitFor([&] {
            Message frame;
            while (session.connection.poll(frame)) {
                if (frame.type == MsgType::HistoryEnd) {
                    finished = true;
                    return true;
                }
                if (frame.type != MsgType::PenguinSprite) continue;
                CHECK(!frame.fields.empty());
                auto& transfer = transfers[frame.fields[0]];
                CHECK(transfer.accept(frame, true));
                if (transfer.complete()) {
                    if (transfer.data().empty()) sprites.erase(frame.fields[0]);
                    else sprites[frame.fields[0]] = transfer.data();
                }
            }
            return false;
        }));
        CHECK(finished);
        return sprites;
    };
    upload(alice, aliceSprite);
    receive(bob, "sprite_alice", aliceSprite);
    upload(bob, bobSprite);
    receive(alice, "sprite_bob", bobSprite);

    RawSession directNewcomer(serverPort);
    signIn(directNewcomer, "sprite_direct_new");
    const auto directSprites = snapshot(directNewcomer);
    CHECK(directSprites.size() == 2);
    CHECK(directSprites.at("sprite_alice") == aliceSprite);
    CHECK(directSprites.at("sprite_bob") == bobSprite);
    RawSession relayNewcomer(relayPort);
    signIn(relayNewcomer, "sprite_relay_new");
    CHECK(snapshot(relayNewcomer) == directSprites);

    const std::string replacement(chat::kImageChunkSize + 1, 'r');
    const auto update = chat::spriteMessages(replacement);
    CHECK(alice.connection.send(update.front()));
    // Ensure the first chunk was processed before the newcomer joins.
    CHECK(alice.connection.send(Message{MsgType::FetchHistory, {}}));
    alice.next(MsgType::HistoryEnd);
    RawSession duringUpload(serverPort);
    signIn(duringUpload, "sprite_during");
    CHECK(snapshot(duringUpload).at("sprite_alice") == aliceSprite);
    CHECK(alice.connection.send(update.back()));
    receive(bob, "sprite_alice", replacement);

    upload(bob, {});
    receive(alice, "sprite_bob", {});
    RawSession afterReset(relayPort);
    signIn(afterReset, "sprite_after_reset");
    const auto resetSprites = snapshot(afterReset);
    CHECK(resetSprites.size() == 1 && resetSprites.at("sprite_alice") == replacement);

    // A claimed sender is not part of the client message format.
    RawSession attacker(serverPort);
    signIn(attacker, "sprite_attacker");
    CHECK(attacker.connection.send(Message{MsgType::PenguinSprite,
                                          {"sprite_alice", "0", "1", "fake"}}));
    CHECK(attacker.next(MsgType::Error).fields.at(0) == "invalid sprite transfer");
    alice.connection.stop();
    CHECK(waitFor([&] {
        Message message;
        while (bob.connection.poll(message)) {
            if (message.type == MsgType::PeerLeft &&
                message.fields == std::vector<std::string>{"sprite_alice"}) return true;
        }
        return false;
    }));
    RawSession rejoined(serverPort);
    signIn(rejoined, "sprite_alice");
    RawSession afterDeparture(serverPort);
    signIn(afterDeparture, "sprite_after_leave");
    CHECK(snapshot(afterDeparture).empty());
    upload(rejoined, aliceSprite);
    receive(bob, "sprite_alice", aliceSprite);
}

void testRelayDuplicateNames(std::uint16_t relayPort) {
    using Fields = std::vector<std::string>;
    RawSession first(relayPort);
    signIn(first, "relay_same");
    RawSession second(relayPort);
    CHECK(second.connection.send(chat::Message{chat::MsgType::Login, {"relay_same", "0", ""}}));
    CHECK(second.next(chat::MsgType::LoginOk).fields.at(0) == "relay_same-2");
    CHECK(!first.connection.failed());

    // This name sorts before both duplicates, so its mesh dials them. If
    // removing the suffixed user also removes the original, it cannot redial.
    RawSession observer(relayPort);
    signIn(observer, "relay_observer");
    auto deliver = [](RawSession& from, const std::string& sender, RawSession& to,
                      const std::string& body) {
        CHECK(waitFor([&] {
            CHECK(!from.connection.failed());
            CHECK(!to.connection.failed());
            CHECK(from.connection.send(chat::Message{chat::MsgType::PeerChat,
                                                      {sender, "123456789", body, "pink"}}));
            chat::Message message;
            while (to.connection.poll(message)) {
                if (message.type == chat::MsgType::PeerChat &&
                    message.fields == Fields{sender, "123456789", body, "pink"}) return true;
            }
            return false;
        }));
    };
    deliver(first, "relay_same", second, "first to second");
    deliver(second, "relay_same-2", first, "second to first");
    deliver(observer, "relay_observer", first, "observer before departure");

    second.connection.stop();
    CHECK(waitFor([&] {
        chat::Message message;
        while (observer.connection.poll(message)) {
            if (message.type == chat::MsgType::PeerLeft &&
                message.fields == Fields{"relay_same-2"}) return true;
        }
        return false;
    }));
    deliver(observer, "relay_observer", first, "observer after departure");
    deliver(first, "relay_same", observer, "first after departure");

    // Reconnecting with the original requested name creates another session
    // without stealing the surviving client's socket or server identity.
    RawSession reconnect(relayPort);
    CHECK(reconnect.connection.send(chat::Message{chat::MsgType::Login, {"relay_same", "0", ""}}));
    CHECK(reconnect.next(chat::MsgType::LoginOk).fields.at(0) == "relay_same-2");
    deliver(first, "relay_same", reconnect, "first after reconnect");
    deliver(reconnect, "relay_same-2", first, "reconnected second");
}

void testScreenSharing(std::uint16_t serverPort, std::uint16_t relayPort) {
    using Fields = std::vector<std::string>;

    RawSession sharer(serverPort);
    signIn(sharer, "screen_sharer");
    RawSession viewer(relayPort);
    signIn(viewer, "screen_viewer");
    RawSession bystander(serverPort);
    signIn(bystander, "screen_bystander");

    CHECK(sharer.connection.send(chat::Message{chat::MsgType::ScreenShare, {"1"}}));
    CHECK(viewer.next(chat::MsgType::ScreenShare).fields == (Fields{"screen_sharer", "1"}));
    CHECK(bystander.next(chat::MsgType::ScreenShare).fields == (Fields{"screen_sharer", "1"}));

    // Someone who joins mid-share learns about it at sign-in.
    {
        RawSession late(serverPort);
        signIn(late, "screen_late");
        CHECK(late.next(chat::MsgType::ScreenShare).fields == (Fields{"screen_sharer", "1"}));
    }

    // Frames reach a viewer through the relay once it asks to watch. The
    // request races the frames, so keep sending until one arrives.
    CHECK(viewer.connection.send(chat::Message{chat::MsgType::ScreenWatch, {"screen_sharer", "1"}}));
    const chat::Message frame{chat::MsgType::ScreenFrame, {"7", "0", "1", "1", "h264 bytes"}};
    chat::Message received;
    CHECK(waitFor([&] {
        CHECK(sharer.connection.send(frame));
        chat::Message message;
        while (viewer.connection.poll(message)) {
            if (message.type == chat::MsgType::ScreenFrame) {
                received = message;
                return true;
            }
        }
        return false;
    }));
    CHECK(received.fields == (Fields{"screen_sharer", "7", "0", "1", "1", "h264 bytes"}));

    // Nobody receives a stream they did not ask to watch.
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    chat::Message message;
    while (bystander.connection.poll(message)) {
        CHECK(message.type != chat::MsgType::ScreenFrame);
    }

    CHECK(sharer.connection.send(chat::Message{chat::MsgType::ScreenShare, {"0"}}));
    CHECK(viewer.next(chat::MsgType::ScreenShare).fields == (Fields{"screen_sharer", "0"}));
    CHECK(bystander.next(chat::MsgType::ScreenShare).fields == (Fields{"screen_sharer", "0"}));

    // Leaving ends a share too.
    CHECK(sharer.connection.send(chat::Message{chat::MsgType::ScreenShare, {"1"}}));
    CHECK(bystander.next(chat::MsgType::ScreenShare).fields == (Fields{"screen_sharer", "1"}));
    sharer.connection.stop();
    CHECK(bystander.next(chat::MsgType::ScreenShare).fields == (Fields{"screen_sharer", "0"}));
}

// Drawing is an opaque peer channel. The discovery server only introduces
// peers; no annotation messages go to it, and it owns no drawing state.
void testRelayPeerData(std::uint16_t serverPort, std::uint16_t relayPort) {
    using chat::Message;
    using chat::MsgType;
    RawSession alice(relayPort), bob(relayPort), bystander(relayPort);
    signIn(alice, "data_alice");
    signIn(bob, "data_bob");
    signIn(bystander, "data_bystander");
    const std::string payload("opaque\0drawing bytes", 20);
    const Message forged = chat::peerDataMessage(
        {"forged_sender", "data_bob", "screen-drawing", payload});
    Message received;
    CHECK(waitFor([&] {
        CHECK(alice.connection.send(forged));
        Message message;
        while (bob.connection.poll(message)) {
            if (message.type == MsgType::PeerData) {
                received = message;
                return true;
            }
        }
        return false;
    }));
    chat::PeerData data;
    CHECK(chat::parsePeerData(received, data));
    CHECK(data.sender == "data_alice" && data.target == "data_bob");
    CHECK(data.channel == "screen-drawing" && data.data == payload);
    // Targeting is generic; the relay does not interpret the channel or bytes.
    CHECK(bob.connection.send(
        chat::peerDataMessage({"spoof", "data_alice", "another-channel", "reply"})));
    CHECK(chat::parsePeerData(alice.next(MsgType::PeerData), data));
    CHECK(data.sender == "data_bob" && data.data == "reply" && data.channel == "another-channel");
    // The reply fences forwarding of the first message; nobody else receives it.
    Message unexpected;
    while (bystander.connection.poll(unexpected))
        CHECK(unexpected.type != MsgType::PeerData);

    // A real peer mesh exchanges the same opaque data with a hosted user.
    chat::PeerNetwork direct;
    std::string error;
    CHECK(direct.start(0, error));
    direct.setMyName("data_direct");
    RawSession roster(serverPort);
    CHECK(roster.connection.send(
        {MsgType::Login, {"data_direct", std::to_string(direct.port()), ""}}));
    CHECK(roster.next(MsgType::LoginOk).fields[0] == "data_direct");
    chat::PeerAddress address;
    CHECK(waitFor([&] {
        Message message;
        while (roster.connection.poll(message)) {
            if (chat::parsePeerAddress(message, address))
                direct.addPeer(address.name, address.host, address.port, address.advertised);
        }
        chat::PeerNetwork::Event event;
        while (direct.poll(event)) {
        }
        return direct.connectedTo("data_alice");
    }));
    CHECK(direct.sendData("data_alice", "screen-drawing", "direct to relay"));
    CHECK(chat::parsePeerData(alice.next(MsgType::PeerData), data));
    CHECK(data.sender == "data_direct" && data.data == "direct to relay");
    CHECK(alice.connection.send(
        chat::peerDataMessage({"spoof", "data_direct", "screen-drawing", "relay to direct"})));
    bool delivered = false;
    CHECK(waitFor([&] {
        chat::PeerNetwork::Event event;
        while (direct.poll(event)) {
            if (event.kind == chat::PeerNetwork::Event::Kind::Data) {
                CHECK(event.data.sender == "data_alice" && event.data.data == "relay to direct");
                delivered = true;
            }
        }
        return delivered;
    }));
}

// Image chunks cross the mesh into and out of the relay. The relay stamps
// each chunk with its hosted user's name, whatever the frame claims.
void testRelayImages(std::uint16_t relayPort) {
    using Fields = std::vector<std::string>;
    RawSession sender(relayPort);
    signIn(sender, "image_sender");
    RawSession receiver(relayPort);
    signIn(receiver, "image_receiver");

    const std::string bytes(chat::kImageChunkSize, '\x7f');
    const chat::Message spoofed{chat::MsgType::PeerImage,
                                {"someone_else", "123456789", "mint", "42", "0", "2", bytes}};
    const Fields expected{"image_sender", "123456789", "mint", "42", "0", "2", bytes};
    CHECK(waitFor([&] {
        CHECK(sender.connection.send(spoofed));
        chat::Message message;
        while (receiver.connection.poll(message)) {
            if (message.type == chat::MsgType::PeerImage) {
                CHECK(message.fields == expected);
                return true;
            }
        }
        return false;
    }));

    // Invalid chunks go nowhere: past the retries of transfer 42 above, the
    // valid chunk sent after an invalid one arrives first.
    CHECK(sender.connection.send(chat::Message{
        chat::MsgType::PeerImage, {"image_sender", "1", "mint", "43", "2", "2", "x"}}));
    CHECK(sender.connection.send(chat::Message{
        chat::MsgType::PeerImage, {"image_sender", "1", "mint", "44", "1", "2", "last"}}));
    chat::Message next;
    do {
        next = receiver.next(chat::MsgType::PeerImage);
    } while (next.fields.at(3) == "42");
    CHECK(next.fields.at(3) == "44");
}

void testHistoryAndRoster(std::uint16_t serverPort) {
    RawSession dave(serverPort);
    CHECK(dave.connection.send(chat::Message{chat::MsgType::Login, {" alice ", "1", ""}}));
    const chat::Message ok = dave.next(chat::MsgType::LoginOk);
    // alice is taken, so the server picks a unique variant of the name.
    CHECK(ok.fields.at(0) == "alice-2");

    std::vector<std::string> peers;
    const bool sawEveryone = waitFor([&] {
        chat::Message message;
        while (dave.connection.poll(message)) {
            if (message.type == chat::MsgType::Peer) {
                chat::PeerAddress peer;
                CHECK(chat::parsePeerAddress(message, peer));
                peers.push_back(peer.name);
                if (peer.name == "carol") {
                    // The relay advertised itself as carol's address.
                    CHECK(peer.advertised);
                    CHECK(peer.host == "127.0.0.1");
                    CHECK(peer.port == gRelay.port);
                }
            }
        }
        return peers.size() == 3;
    });
    CHECK(sawEveryone);
    std::sort(peers.begin(), peers.end());
    CHECK((peers == std::vector<std::string>{"alice", "bob", "carol"}));

    CHECK(dave.connection.send(chat::Message{chat::MsgType::Store,
        {"123456789", "legacy message must not be retained", "20"}}));
    CHECK(dave.connection.send(chat::Message{chat::MsgType::FetchHistory, {}}));
    std::vector<chat::Message> history;
    CHECK(waitFor([&] {
        chat::Message message;
        while (dave.connection.poll(message)) {
            if (message.type == chat::MsgType::History) {
                history.push_back(message);
            }
            else if (message.type == chat::MsgType::HistoryEnd) {
                return true;
            }
        }
        return false;
    }));
    CHECK(history.empty());
}

} // namespace

int main(int argc, char** argv) {
    CHECK(argc == 4);
    const std::string serverPath = argv[1];
    const std::string relayPath = argv[2];
    gCheckCleanup = killChildren;
    signal(SIGPIPE, SIG_IGN);

    spawnListening(gServer, {serverPath, "--port", "0", "--db", ":memory:"});
    spawnListening(gRelay,
                   {relayPath, "--host", "127.0.0.1", "--port", std::to_string(gServer.port),
                    "--listen", "0", "--advertise", "127.0.0.1"});

    const std::string serverAddress = "127.0.0.1:" + std::to_string(gServer.port);
    const std::string relayAddress = "127.0.0.1:" + std::to_string(gRelay.port);

    testServerRejectsUnexpectedFrames(gServer.port);
    testVoiceAndRelayHealth(gServer.port, gRelay.port);
    testPersonalSprites(gServer.port, gRelay.port);
    testScreenSharing(gServer.port, gRelay.port);
    testRelayPeerData(gServer.port, gRelay.port);
    testRelayDuplicateNames(gRelay.port);
    testRelayImages(gRelay.port);

    {
        disquisition::Client alice(serverAddress);
        alice.onMessage(aliceHandler);
        alice.connect();
        alice.setName("alice");

        disquisition::Client bob(serverAddress, disquisition::Client::DIRECT);
        bob.onMessage(bobHandler);
        bob.connect();
        bob.setName("bob");

        disquisition::Client carol(relayAddress, disquisition::Client::RELAY);
        carol.onMessage(carolHandler);
        carol.connect();
        carol.setName("carol");
        carol.setColor(20);

        // Direct peer-to-peer links in both directions.
        deliverEventually(alice, "alice", gBobInbox, "hello from alice");
        deliverEventually(bob, "bob", gAliceInbox, "hello from bob");
        // Into and out of the relay.
        deliverEventually(carol, "carol", gAliceInbox, "hello from carol");
        deliverEventually(carol, "carol", gBobInbox, "hello from carol");
        deliverEventually(alice, "alice", gCarolInbox, "hello from alice");

        // Nobody hears their own messages echoed back.
        CHECK(!gAliceInbox.contains("alice", "hello from alice"));
        CHECK(!gCarolInbox.contains("carol", "hello from carol"));

        testHistoryAndRoster(gServer.port);

        carol.disconnect();
        bob.disconnect();
        alice.disconnect();
    }

    stopChild(gRelay);
    stopChild(gServer);
    std::puts("end_to_end_test: ok");
    return 0;
}
