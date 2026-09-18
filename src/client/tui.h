#pragma once

#include <ncurses.h>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include "client/connection.h"
#include "client/peer_network.h"

namespace chat {

// Minimal ncurses chat interface: a header bar, a scrolling message pane and a
// single input line. The server connection only carries sign-in, discovery and
// storage traffic; live chat arrives as PeerNetwork events drained once per UI
// tick.
class Tui {
public:
    Tui(Connection& connection, PeerNetwork& peers);
    ~Tui();

    int run(const std::string& initialName, const std::string& host, std::uint16_t port,
            const std::string& advertiseHost, bool useRelay = false);

private:
    struct Entry {
        std::string prefix;
        std::string body;
        int color = 0;
    };

    struct Row {
        std::string text;
        int color = 0;
    };

    bool start();
    void stop();
    void layout();
    void draw();
    void drawHeader();
    void drawMessages();
    void drawInput();

    void append(const std::string& prefix, const std::string& body, int color);
    void appendSystem(const std::string& text, int color = 3);
    void rebuildRows();
    std::size_t maxScroll() const;
    const char* prompt() const;
    int pairFor(const std::string& color) const;

    void drainIncoming();
    void drainPeers();
    bool remember(const std::string& sender, const std::string& timestamp, const std::string& body);
    void sendLogin();
    void deliver(const std::string& line);
    void monitorServer();
    void handleKey(int key);
    void submit();
    void runCommand(const std::string& command);

    Connection& connection_;
    PeerNetwork& peers_;
    std::string host_;
    std::uint16_t port_ = 0;
    std::string advertiseHost_;
    // True when this client reaches the mesh through a relay: the relay owns
    // the peer connections, so chat arrives on connection_ (carrying a sender)
    // rather than as PeerNetwork events, and the local mesh stays passive.
    bool useRelay_ = false;

    WINDOW* header_ = nullptr;
    WINDOW* messages_ = nullptr;
    WINDOW* input_ = nullptr;

    int width_ = 0;
    int messageHeight_ = 0;

    std::vector<Entry> entries_;
    std::vector<Row> rows_;
    std::size_t scroll_ = 0;

    std::string text_;
    std::size_t cursor_ = 0;

    std::string name_;
    std::vector<std::string> users_;
    std::vector<std::string> pending_;
    std::string color_;
    // Numeric xterm-256 choices get a color pair only when someone uses one.
    mutable std::map<std::string, int> customColorPairs_;
    mutable int nextCustomColorPair_ = 27;

    // (sender, timestamp, body) triples already shown, so a message arriving
    // both through history and over the mesh is only displayed once.
    std::set<std::string> seen_;

    std::string status_ = "connecting";
    int statusColor_ = 3;

    bool signedIn_ = false;
    bool loginSent_ = false;
    bool historyOpen_ = false;
    bool historyPending_ = false;
    bool serverLost_ = false;
    // True once the current server connection has completed a login. The
    // transport can be up while this is false (reconnect raced ahead of the
    // Login round trip), and nothing may be stored or fetched until it flips.
    bool serverReady_ = false;
    bool quit_ = false;
    bool dirty_ = true;

    // The monitor thread only ever touches the server connection and these
    // atomics; all UI state stays on the main thread.
    std::atomic<bool> serverBack_ {false};
    std::atomic<bool> monitorRunning_ {false};
    // Bumped by the monitor on every successful (re)connect. A Login belongs
    // to exactly one transport: it is sent only when loginGen_ lags behind,
    // so a flapping server can neither starve nor duplicate the login.
    std::atomic<std::uint64_t> transportGen_ {1};
    std::uint64_t loginGen_ = 0;
    std::mutex monitorMutex_;
    std::condition_variable monitorCv_;
};

}  // namespace chat
