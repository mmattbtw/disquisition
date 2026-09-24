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
#include "client/options.h"
#include "client/peer_network.h"

namespace chat {

// The ncurses chat client: a header bar, a scrolling message pane and an
// input line.
//
// Everything runs on the UI thread except a monitor thread that reconnects
// the server (or relay) connection. The two communicate only through the
// atomics below.
class Tui {
public:
    Tui();
    ~Tui();

    int run(const ClientOptions& options);

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

    // Screen
    bool start();
    void stop();
    void layout();
    void draw();
    void drawHeader();
    void drawMessages();
    void drawInput();
    const char* prompt() const;
    std::size_t maxScroll() const;
    int pairFor(const std::string& color) const;

    // Message pane
    void append(const std::string& prefix, const std::string& body, int color);
    void appendSystem(const std::string& text);
    void appendSystem(const std::string& text, int color);
    void rebuildRows();
    void showGreeting();
    void showChat(const std::string& sender, std::int64_t timestamp, const std::string& body,
                  const std::string& color);
    void showHistory(const Message& message);
    bool remember(const std::string& sender, const std::string& timestamp, const std::string& body);

    // Session
    bool startPeers();
    void connectUpstream();
    void setStatus(const std::string& text, int color);
    void signIn();
    void sendLogin();
    void onSignedIn(const Message& message);
    void deliver(const std::string& line);
    void checkConnectionLost();
    void resumeAfterReconnect();
    void requestHistoryWhenSettled();
    void drainServer();
    void drainPeers();

    // Input
    void handleKey(int key);
    void clearInput();
    void submit();
    void runCommand(const std::string& command);
    void listUsers();
    void changeColor(const std::string& color);

    // Monitor thread
    void monitorServer();
    void reconnect();
    void returnToRelay();
    void useTransport(Connection* connection, bool throughRelay);
    void markTransportUp();

    ClientOptions options_;

    // Transport
    PeerNetwork peers_;
    Connection connection_;       // server, or relay when options_.useRelay
    Connection directConnection_; // server, while --leak-my-ip bypasses a dead relay
    std::atomic<Connection*> activeConnection_{nullptr};
    std::atomic<bool> relayActive_{false};

    // Screen
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
    std::string status_ = "connecting";
    int statusColor_ = 0;
    mutable std::map<std::string, int> customColorPairs_;
    mutable int nextCustomColorPair_ = 27;
    bool dirty_ = true;
    bool quit_ = false;

    // Session
    std::string name_;
    std::string relayRequestedName_;
    std::string color_;
    std::vector<std::string> users_;
    std::vector<std::string> pending_; // typed before sign-in completed
    std::set<std::string> seen_;       // dedupes chat seen both live and in history
    bool signedIn_ = false;
    bool loginSent_ = false;
    bool historyOpen_ = false;
    bool historyPending_ = false;
    bool serverLost_ = false;
    // The current connection has completed a login; until then nothing may
    // be stored or fetched, even if the socket is up.
    bool serverReady_ = false;

    // Monitor thread. transportGen_ increases on every reconnect; a Login is
    // sent only when loginGen_ lags behind it, so each transport gets exactly
    // one.
    std::atomic<bool> monitorRunning_{false};
    std::atomic<bool> serverBack_{false};
    std::atomic<bool> transportChanged_{false};
    std::atomic<std::uint64_t> transportGen_{1};
    std::uint64_t loginGen_ = 0;
    std::mutex monitorMutex_;
    std::condition_variable monitorCv_;
};

} // namespace chat
