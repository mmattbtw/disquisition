// Black-box test of the whole system: it launches the real server and relay
// executables, connects library clients to them, and checks that chat
// travels peer to peer, through the relay, and into the server's history.
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
    auto stored = [&](const std::string& sender, const std::string& body) {
        return std::any_of(history.begin(), history.end(), [&](const chat::Message& message) {
            return message.fields.size() == 4 && message.fields[1] == sender &&
                   message.fields[2] == body;
        });
    };
    CHECK(stored("alice", "hello from alice"));
    CHECK(stored("bob", "hello from bob"));
    // carol's copy reached the server through the relay.
    CHECK(stored("carol", "hello from carol"));
}

} // namespace

int main(int argc, char** argv) {
    CHECK(argc == 4);
    const std::string serverPath = argv[1];
    const std::string relayPath = argv[2];
    const std::string database = std::string(argv[3]) + "/end_to_end_test.db";
    for (const char* suffix : {"", "-wal", "-shm"}) {
        std::remove((database + suffix).c_str());
    }

    gCheckCleanup = killChildren;
    signal(SIGPIPE, SIG_IGN);

    spawnListening(gServer, {serverPath, "--port", "0", "--db", database});
    spawnListening(gRelay,
                   {relayPath, "--host", "127.0.0.1", "--port", std::to_string(gServer.port),
                    "--listen", "0", "--advertise", "127.0.0.1"});

    const std::string serverAddress = "127.0.0.1:" + std::to_string(gServer.port);
    const std::string relayAddress = "127.0.0.1:" + std::to_string(gRelay.port);

    testServerRejectsUnexpectedFrames(gServer.port);

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
    for (const char* suffix : {"", "-wal", "-shm"}) {
        std::remove((database + suffix).c_str());
    }
    std::puts("end_to_end_test: ok");
    return 0;
}
