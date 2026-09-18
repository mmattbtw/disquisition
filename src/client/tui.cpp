#include "client/tui.h"

#include <algorithm>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <chrono>
#include <random>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace chat {
namespace {

constexpr std::size_t kMaxEntries = 1000;
constexpr std::size_t kMaxInputLength = 2000;
constexpr std::size_t kMaxSeen = 2000;
constexpr int kReconnectIntervalMs = 3000;
constexpr int kReconnectTimeoutMs = 3000;

// Color pair numbers registered in Tui::start().
constexpr int kColorOwn = 1;
constexpr int kColorOther = 2;
constexpr int kColorSystem = 3;
constexpr int kColorHeader = 4;
constexpr int kColorGood = 5;
constexpr int kColorBad = 6;
// Own messages always render in full white, independent of the chosen shade.
constexpr int kColorSelf = 7;
// First color pair number used for per-user palette colors.
constexpr int kFirstUserColor = 20;

// xterm-256 index for the brightest white; plain COLOR_WHITE on limited
// terminals.
constexpr int kSelfWhite = 231;

// Maps a palette name to the basic ncurses foreground used on limited terminals.
// Order must line up with kColorNames.
constexpr int kBasicUserColorValues[] = {
    COLOR_MAGENTA, COLOR_CYAN, COLOR_YELLOW, COLOR_BLUE, COLOR_MAGENTA, COLOR_CYAN, COLOR_BLUE};

// Soft xterm-256 colors keep the palette playful without borrowing the
// neutral used for system notices. Order must line up with kColorNames.
constexpr int kCandyUserColorValues[] = {211, 121, 229, 111, 183, 159, 216};
constexpr int kCandySystemColor = 250;

volatile std::sig_atomic_t gInterrupted = 0;

void handleInterrupt(int) {
    gInterrupted = 1;
}

std::int64_t nowSeconds() {
    return static_cast<std::int64_t>(std::time(nullptr));
}

std::string dedupeKey(const std::string& sender, const std::string& timestamp,
                      const std::string& body) {
    return sender + '\x1f' + timestamp + '\x1f' + body;
}

std::string formatTime(const std::string& epochSeconds) {
    const long long seconds = std::strtoll(epochSeconds.c_str(), nullptr, 10);
    const std::time_t stamp = static_cast<std::time_t>(seconds);
    std::tm local {};
    if (localtime_r(&stamp, &local) == nullptr) {
        return "--:--";
    }
    char buffer[16] = {0};
    std::strftime(buffer, sizeof(buffer), "%H:%M", &local);
    return buffer;
}

std::string trim(const std::string& text) {
    const auto begin = text.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos) {
        return "";
    }
    const auto end = text.find_last_not_of(" \t\r\n");
    return text.substr(begin, end - begin + 1);
}

// Picks a random shade from the named palette. Every name maps to a soft
// xterm-256 color, so a fresh connection never collides with the reserved
// system colors.
std::string randomColorName() {
    static std::mt19937 engine(std::random_device {}());
    std::uniform_int_distribution<std::size_t> pick(0, kColorCount - 1);
    return kColorNames[pick(engine)];
}

std::string paletteList() {
    std::string list;
    for (std::size_t index = 0; index < kColorCount; ++index) {
        if (index != 0) {
            list += ", ";
        }
        list += kColorNames[index];
    }
    return list;
}

// Greedy word wrap with a hanging indent as deep as `prefix`, so a wrapped
// message lines up under the body rather than the timestamp.
std::vector<std::string> wrapText(const std::string& prefix, const std::string& body, int width) {
    std::vector<std::string> lines;
    if (width <= 1) {
        lines.push_back(prefix + body);
        return lines;
    }

    std::string indent;
    if (static_cast<int>(prefix.size()) < width) {
        indent.assign(prefix.size(), ' ');
    }

    std::vector<std::string> words;
    std::istringstream stream(body);
    std::string word;
    while (stream >> word) {
        words.push_back(word);
    }
    if (words.empty()) {
        lines.push_back(prefix);
        return lines;
    }

    std::string line = prefix;
    bool fresh = true;
    for (const std::string& original : words) {
        std::string current = original;
        for (;;) {
            if (fresh && line.size() >= static_cast<std::size_t>(width)) {
                line.clear();
            }
            const std::size_t separator = fresh ? 0 : 1;
            const std::size_t used = line.size() + separator;
            const std::size_t room = used < static_cast<std::size_t>(width)
                                         ? static_cast<std::size_t>(width) - used
                                         : 0;
            if (current.size() <= room) {
                if (!fresh) {
                    line += ' ';
                }
                line += current;
                fresh = false;
                break;
            }
            if (!fresh) {
                lines.push_back(line);
                line = indent;
                fresh = true;
                continue;
            }
            const std::size_t take = room > 0 ? room : 1;
            line += current.substr(0, take);
            current.erase(0, take);
            lines.push_back(line);
            line = indent;
            fresh = true;
        }
    }
    if (!line.empty()) {
        lines.push_back(line);
    }
    return lines;
}

}  // namespace

Tui::Tui(Connection& connection, PeerNetwork& peers) : connection_(connection), peers_(peers) {}

Tui::~Tui() {
    stop();
}

bool Tui::start() {
    if (initscr() == nullptr) {
        return false;
    }
    cbreak();
    noecho();
    keypad(stdscr, TRUE);
    curs_set(1);
    if (has_colors()) {
        start_color();
        use_default_colors();
        const bool hasCandyPalette = COLORS >= 256;
        init_pair(kColorOwn, COLOR_CYAN, -1);
        init_pair(kColorOther, COLOR_MAGENTA, -1);
        init_pair(kColorSystem, hasCandyPalette ? kCandySystemColor : COLOR_WHITE, -1);
        init_pair(kColorHeader, COLOR_BLACK, COLOR_CYAN);
        init_pair(kColorGood, COLOR_GREEN, -1);
        init_pair(kColorBad, COLOR_RED, -1);
        init_pair(kColorSelf, hasCandyPalette ? kSelfWhite : COLOR_WHITE, -1);
        for (std::size_t index = 0; index < kColorCount; ++index) {
            const int color = hasCandyPalette ? kCandyUserColorValues[index]
                                               : kBasicUserColorValues[index];
            init_pair(kFirstUserColor + static_cast<int>(index), color, -1);
        }
    }

    struct sigaction action {};
    action.sa_handler = handleInterrupt;
    sigemptyset(&action.sa_mask);
    sigaction(SIGINT, &action, nullptr);
    sigaction(SIGTERM, &action, nullptr);
    return true;
}

void Tui::stop() {
    if (messages_ != nullptr) {
        delwin(messages_);
        messages_ = nullptr;
    }
    if (input_ != nullptr) {
        delwin(input_);
        input_ = nullptr;
    }
    if (header_ != nullptr) {
        delwin(header_);
        header_ = nullptr;
    }
    if (stdscr != nullptr) {
        curs_set(1);
        endwin();
    }
}

int Tui::run(const std::string& initialName, const std::string& host, std::uint16_t port,
             const std::string& advertiseHost, bool useRelay, const std::string& relayHost,
             std::uint16_t relayPort, bool leakMyIp) {
    host_ = host;
    port_ = port;
    relayHost_ = relayHost;
    relayPort_ = relayPort;
    advertiseHost_ = advertiseHost;
    useRelay_ = useRelay;
    leakMyIp_ = leakMyIp;
    relayActive_.store(useRelay);
    activeConnection_.store(&connection_);
    if (useRelay_) {
        peers_.setPassive(true);
    }
    name_ = trim(initialName);
    relayRequestedName_ = name_;
    color_ = randomColorName();

    if (!start()) {
        std::fprintf(stderr, "cannot initialise terminal\n");
        return 1;
    }

    peers_.setMyAdvertised(!advertiseHost_.empty());

    layout();

    const std::string endpoint = useRelay_ ? relayHost_ + ":" + std::to_string(relayPort_)
                                           : host_ + ":" + std::to_string(port_);
    std::string greeting;
    if (connection_.failed()) {
        greeting = (useRelay_ ? "relay " : "server ") + endpoint + " is unreachable; retrying";
    } else if (useRelay_) {
        greeting = "connected to relay " + endpoint;
    } else {
        greeting = "connected to " + endpoint + " (peer port " + std::to_string(peers_.port()) + ")";
        if (!advertiseHost_.empty()) {
            greeting += ", advertising " + advertiseHost_;
        }
    }
    // The relay decides our name, so don't announce a guess before it replies.
    if (!name_.empty() && !useRelay_) {
        greeting += " as " + name_;
    }
    appendSystem(greeting, kColorGood);

    if (!name_.empty() && !connection_.failed()) {
        loginGen_ = transportGen_.load();
        loginSent_ = true;
        status_ = "signing in";
        sendLogin();
    } else if (name_.empty()) {
        appendSystem("type a name and press enter to join");
    }

    monitorRunning_.store(true);
    std::thread monitor([this] { monitorServer(); });

    while (!quit_) {
        if (gInterrupted != 0) {
            quit_ = true;
        }

        // Notice a dead server before draining, so stale frames queued behind
        // the disconnect are purged instead of processed. Re-login re-syncs
        // the roster, users and history from scratch. Losing the server only
        // costs discovery, storage and history: the mesh keeps working and
        // the monitor keeps retrying, so this is not fatal.
        Connection* active = activeConnection_.load();
        if (active->failed() && !serverLost_) {
            Message stale;
            while (active->poll(stale)) {
            }
            serverLost_ = true;
            serverReady_ = false;
            status_ = "server offline";
            statusColor_ = kColorBad;
            appendSystem(relayActive_.load() ? "relay connection lost; reconnecting"
                                   : "server connection lost; peer-to-peer chat still works",
                         kColorBad);
            dirty_ = true;
        }

        drainIncoming();
        drainPeers();

        // The monitor re-established the transport; sign in on it from here,
        // where all the UI state lives. The generation check sends exactly
        // one Login per transport no matter which thread observes it first.
        active = activeConnection_.load();
        if (serverBack_.exchange(false) && !active->failed()) {
            serverLost_ = false;
            if (transportChanged_.exchange(false)) {
                appendSystem(relayActive_.load() ? "relay connection restored"
                                                 : "relay unavailable; connected directly to " +
                                                       host_ + ":" + std::to_string(port_),
                             kColorGood);
            }
            if (!name_.empty() && loginGen_ != transportGen_.load()) {
                loginGen_ = transportGen_.load();
                loginSent_ = true;
                status_ = "signing in";
                statusColor_ = kColorSystem;
                sendLogin();
            } else if (name_.empty()) {
                status_ = "connecting";
                statusColor_ = kColorSystem;
            }
            dirty_ = true;
        }

        // Fetch recent messages only once the mesh has settled, so nothing a
        // reachable peer already stored can slip between the fetch and the
        // live stream. Anything arriving through both is deduplicated.
        if (historyPending_ && serverReady_ && peers_.settled()) {
            historyPending_ = false;
            activeConnection_.load()->send(Message {MsgType::FetchHistory, {}});
        }

        if (dirty_) {
            draw();
            dirty_ = false;
        }
        if (quit_) {
            break;
        }

        const int key = wgetch(input_);
        if (key != ERR) {
            handleKey(key);
            dirty_ = true;
        }
    }

    monitorRunning_.store(false);
    monitorCv_.notify_all();
    monitor.join();

    stop();
    return activeConnection_.load()->failed() && !signedIn_ ? 1 : 0;
}

void Tui::monitorServer() {
    for (;;) {
        {
            std::unique_lock<std::mutex> lock(monitorMutex_);
            monitorCv_.wait_for(lock, std::chrono::milliseconds(kReconnectIntervalMs));
            if (!monitorRunning_.load()) {
                return;
            }
        }
        Connection* active = activeConnection_.load();
        if (active->failed()) {
            active->stop();
            std::string error;
            Connection* next = active;
            std::string nextHost = relayActive_.load() ? relayHost_ : host_;
            std::uint16_t nextPort = relayActive_.load() ? relayPort_ : port_;

            if (useRelay_ && leakMyIp_ && relayActive_.load()) {
                next = &directConnection_;
                nextHost = host_;
                nextPort = port_;
            }
            if (next->connectTo(nextHost, nextPort, kReconnectTimeoutMs, error)) {
                next->startReader();
                if (next != active) {
                    activeConnection_.store(next);
                    relayActive_.store(false);
                    peers_.setPassive(false);
                    transportChanged_.store(true);
                }
                transportGen_.fetch_add(1);
                serverBack_.store(true);
            } else if (useRelay_ && leakMyIp_ && relayActive_.load() &&
                       connection_.connectTo(relayHost_, relayPort_, kReconnectTimeoutMs, error)) {
                connection_.startReader();
                transportGen_.fetch_add(1);
                serverBack_.store(true);
            }
        } else if (useRelay_ && leakMyIp_ && !relayActive_.load()) {
            connection_.stop();
            std::string error;
            if (connection_.connectTo(relayHost_, relayPort_, kReconnectTimeoutMs, error)) {
                connection_.startReader();
                activeConnection_.store(&connection_);
                relayActive_.store(true);
                peers_.setPassive(true);
                directConnection_.stop();
                transportChanged_.store(true);
                transportGen_.fetch_add(1);
                serverBack_.store(true);
            }
        }
    }
}

void Tui::layout() {
    int rows = 0;
    int columns = 0;
    getmaxyx(stdscr, rows, columns);

    width_ = columns > 0 ? columns : 80;
    messageHeight_ = std::max(1, rows - 2);

    if (messages_ != nullptr) {
        delwin(messages_);
    }
    if (input_ != nullptr) {
        delwin(input_);
    }
    if (header_ != nullptr) {
        delwin(header_);
    }

    header_ = newwin(1, width_, 0, 0);
    messages_ = newwin(messageHeight_, width_, 1, 0);
    input_ = newwin(1, width_, rows > 1 ? rows - 1 : 0, 0);

    if (input_ != nullptr) {
        keypad(input_, TRUE);
        wtimeout(input_, 80);
    }

    rebuildRows();
    dirty_ = true;
}

void Tui::draw() {
    if (header_ == nullptr || messages_ == nullptr || input_ == nullptr) {
        return;
    }
    drawHeader();
    drawMessages();
    drawInput();
    wnoutrefresh(header_);
    wnoutrefresh(messages_);
    wnoutrefresh(input_);
    doupdate();
}

void Tui::drawHeader() {
    werase(header_);
    if (has_colors()) {
        wbkgd(header_, COLOR_PAIR(kColorHeader));
    }

    std::string left = " disquisition";
    if (signedIn_) {
        left += "  " + name_;
    }
    if (static_cast<int>(left.size()) > width_) {
        left.resize(static_cast<std::size_t>(width_));
    }
    mvwaddstr(header_, 0, 0, left.c_str());

    std::string right = std::to_string(users_.size()) + " online  " + status_ + " ";
    const int start = width_ - static_cast<int>(right.size()) - 1;
    if (start > static_cast<int>(left.size())) {
        if (has_colors()) {
            wattron(header_, COLOR_PAIR(statusColor_));
        }
        mvwaddstr(header_, 0, start, right.c_str());
        if (has_colors()) {
            wattroff(header_, COLOR_PAIR(statusColor_));
        }
    }
}

void Tui::drawMessages() {
    werase(messages_);

    if (scroll_ > maxScroll()) {
        scroll_ = maxScroll();
    }

    const std::size_t total = rows_.size();
    const std::size_t height = static_cast<std::size_t>(messageHeight_);
    const std::size_t top = total > height ? total - height - scroll_ : 0;

    for (int y = 0; y < messageHeight_; ++y) {
        const std::size_t index = top + static_cast<std::size_t>(y);
        if (index >= total) {
            break;
        }
        const Row& row = rows_[index];
        if (has_colors()) {
            wattron(messages_, COLOR_PAIR(row.color));
        }
        mvwaddnstr(messages_, y, 0, row.text.c_str(), width_);
        if (has_colors()) {
            wattroff(messages_, COLOR_PAIR(row.color));
        }
        wclrtoeol(messages_);
    }
}

void Tui::drawInput() {
    werase(input_);

    const char* label = prompt();
    const int labelLength = static_cast<int>(std::strlen(label));
    wattron(input_, A_BOLD);
    mvwaddstr(input_, 0, 0, label);
    wattroff(input_, A_BOLD);

    const int available = std::max(1, width_ - labelLength);
    std::size_t start = 0;
    if (cursor_ >= static_cast<std::size_t>(available)) {
        start = cursor_ - static_cast<std::size_t>(available) + 1;
    }
    const std::string visible = text_.substr(start, static_cast<std::size_t>(available));
    if (has_colors()) {
        wattron(input_, COLOR_PAIR(pairFor(color_)));
    }
    mvwaddstr(input_, 0, labelLength, visible.c_str());
    if (has_colors()) {
        wattroff(input_, COLOR_PAIR(pairFor(color_)));
    }

    const int cursorX = labelLength + static_cast<int>(cursor_ - start);
    wmove(input_, 0, std::min(cursorX, std::max(0, width_ - 1)));
}

const char* Tui::prompt() const {
    if (signedIn_) {
        return "> ";
    }
    return loginSent_ ? ".. " : "name: ";
}

std::size_t Tui::maxScroll() const {
    const std::size_t height = static_cast<std::size_t>(std::max(1, messageHeight_));
    return rows_.size() > height ? rows_.size() - height : 0;
}

void Tui::append(const std::string& prefix, const std::string& body, int color) {
    entries_.push_back(Entry {prefix, body, color});

    if (entries_.size() > kMaxEntries) {
        entries_.erase(entries_.begin(), entries_.begin() + static_cast<std::ptrdiff_t>(200));
        rebuildRows();
        return;
    }

    const std::vector<std::string> wrapped = wrapText(prefix, body, width_);
    const std::size_t added = wrapped.size();
    for (const std::string& line : wrapped) {
        rows_.push_back(Row {line, color});
    }
    // Keep the viewport anchored when the user is reading scrollback.
    if (scroll_ > 0) {
        scroll_ += added;
    }
}

void Tui::appendSystem(const std::string& text, int color) {
    append("", "* " + text, color);
}

int Tui::pairFor(const std::string& color) const {
    for (std::size_t index = 0; index < kColorCount; ++index) {
        if (color == kColorNames[index]) {
            return kFirstUserColor + static_cast<int>(index);
        }
    }

    int colorIndex = 0;
    if (!parseColorIndex(color, colorIndex) || isReservedSystemColor(colorIndex) ||
        !has_colors() || COLORS < 256 || colorIndex >= COLORS) {
        return kColorOther;
    }
    const auto existing = customColorPairs_.find(color);
    if (existing != customColorPairs_.end()) {
        return existing->second;
    }
    if (nextCustomColorPair_ >= COLOR_PAIRS) {
        return kColorOther;
    }
    const int pair = nextCustomColorPair_++;
    if (init_pair(static_cast<short>(pair), static_cast<short>(colorIndex), -1) == ERR) {
        return kColorOther;
    }
    customColorPairs_[color] = pair;
    return pair;
}

void Tui::rebuildRows() {
    rows_.clear();
    for (const Entry& entry : entries_) {
        for (const std::string& line : wrapText(entry.prefix, entry.body, width_)) {
            rows_.push_back(Row {line, entry.color});
        }
    }
}

void Tui::drainIncoming() {
    Message message;
    while (activeConnection_.load()->poll(message)) {
        dirty_ = true;

        switch (message.type) {
            case MsgType::LoginOk: {
                signedIn_ = true;
                loginSent_ = false;
                if (!message.fields.empty()) {
                    name_ = message.fields[0];
                }
                peers_.setMyName(name_);
                historyPending_ = true;
                serverReady_ = true;
                serverLost_ = false;
                status_ = "online";
                statusColor_ = kColorGood;
                appendSystem("you are signed in as " + name_, kColorGood);
                if (!pending_.empty()) {
                    const std::size_t queued = pending_.size();
                    for (const std::string& line : pending_) {
                        deliver(line);
                    }
                    pending_.clear();
                    appendSystem("sent " + std::to_string(queued) + " message(s) typed while signing in");
                }
                break;
            }
            case MsgType::History: {
                if (!historyOpen_) {
                    historyOpen_ = true;
                    appendSystem("recent messages");
                }
                if (message.fields.size() < 4 || !isValidColor(message.fields[3])) {
                    break;
                }
                if (remember(message.fields[1], message.fields[0], message.fields[2])) {
                    append(formatTime(message.fields[0]) + " " + message.fields[1] + ": ",
                           message.fields[2], pairFor(message.fields[3]));
                }
                break;
            }
            case MsgType::HistoryEnd:
                historyOpen_ = false;
                break;
            case MsgType::Peer: {
                if (message.fields.size() < 4) {
                    break;
                }
                std::int64_t peerPort = 0;
                if (!parseInt64(message.fields[2], peerPort) || peerPort < 1 || peerPort > 65535) {
                    break;
                }
                peers_.addPeer(message.fields[0], message.fields[1],
                               static_cast<std::uint16_t>(peerPort), message.fields[3] == "1");
                appendSystem(message.fields[0] + " is online at " + message.fields[1] + ":" +
                             message.fields[2]);
                break;
            }
            case MsgType::PeerJoined: {
                if (message.fields.size() < 4) {
                    break;
                }
                std::int64_t peerPort = 0;
                if (!parseInt64(message.fields[2], peerPort) || peerPort < 1 || peerPort > 65535) {
                    break;
                }
                peers_.addPeer(message.fields[0], message.fields[1],
                               static_cast<std::uint16_t>(peerPort), message.fields[3] == "1");
                appendSystem(message.fields[0] + " joined (" + message.fields[1] + ":" +
                             message.fields[2] + ")");
                break;
            }
            case MsgType::PeerLeft:
                if (!message.fields.empty()) {
                    peers_.removePeer(message.fields[0]);
                    appendSystem(message.fields[0] + " left");
                }
                break;
            case MsgType::System:
                if (!message.fields.empty()) {
                    appendSystem(message.fields[0]);
                }
                break;
            case MsgType::Users:
                users_ = message.fields;
                break;
            // Through a relay every peer arrives over the one relay socket, so
            // chat is attributed from the frame's sender field instead of the
            // connection it came in on.
            case MsgType::PeerChat: {
                if (!relayActive_.load() || message.fields.size() < 4 ||
                    !isValidColor(message.fields[3])) {
                    break;
                }
                const std::string& sender = message.fields[0];
                if (remember(sender, message.fields[1], message.fields[2])) {
                    append(formatTime(message.fields[1]) + " " + sender + ": ",
                           sanitizeBody(message.fields[2]), pairFor(message.fields[3]));
                }
                break;
            }
            case MsgType::Error:
                appendSystem(message.fields.empty() ? "server error" : message.fields[0], kColorBad);
                break;
            default:
                break;
        }
    }
}

void Tui::drainPeers() {
    PeerNetwork::Event event;
    while (peers_.poll(event)) {
        dirty_ = true;

        switch (event.kind) {
            case PeerNetwork::Event::Kind::Chat:
                if (remember(event.name, std::to_string(event.timestamp), event.body)) {
                    append(formatTime(std::to_string(event.timestamp)) + " " + event.name + ": ",
                           event.body, pairFor(event.color));
                }
                break;
            case PeerNetwork::Event::Kind::Join:
                appendSystem("direct link to " + event.name + " is up", kColorGood);
                break;
            case PeerNetwork::Event::Kind::Leave:
                appendSystem("direct link to " + event.name + " is down", kColorBad);
                break;
            case PeerNetwork::Event::Kind::Note:
                appendSystem(event.body, kColorBad);
                break;
        }
    }
}

bool Tui::remember(const std::string& sender, const std::string& timestamp, const std::string& body) {
    if (seen_.size() > kMaxSeen) {
        seen_.clear();
    }
    return seen_.insert(dedupeKey(sender, timestamp, body)).second;
}

void Tui::sendLogin() {
    const bool throughRelay = relayActive_.load();
    if (throughRelay && relayRequestedName_.empty()) {
        relayRequestedName_ = name_;
    }
    activeConnection_.load()->send(
        Message {MsgType::Login,
                 {throughRelay ? relayRequestedName_ : name_, std::to_string(peers_.port()),
                  throughRelay ? "" : advertiseHost_}});
}

void Tui::deliver(const std::string& line) {
    const std::string body = sanitizeBody(line);
    if (body.empty()) {
        return;
    }
    const std::int64_t timestamp = nowSeconds();
    const std::string stamp = std::to_string(timestamp);
    remember(name_, stamp, body);
    append(formatTime(stamp) + " you: ", body, kColorSelf);
    Connection* active = activeConnection_.load();
    if (relayActive_.load()) {
        // The relay fans this out to the mesh on our behalf.
        active->send(Message {MsgType::PeerChat, {name_, stamp, body, color_}});
    } else {
        peers_.sendChat(timestamp, body, color_);
    }
    if (serverReady_ && !active->failed()) {
        active->send(Message {MsgType::Store, {stamp, body, color_}});
    }
}

void Tui::handleKey(int key) {
    switch (key) {
        case 3:  // ^C
        case 4:  // ^D
            quit_ = true;
            return;
        case 12:  // ^L
            clearok(curscr, TRUE);
            return;
        case KEY_RESIZE:
            layout();
            return;
        case KEY_PPAGE:
            scroll_ += static_cast<std::size_t>(std::max(1, messageHeight_ - 1));
            break;
        case KEY_NPAGE:
            scroll_ = scroll_ > static_cast<std::size_t>(messageHeight_)
                          ? scroll_ - static_cast<std::size_t>(messageHeight_)
                          : 0;
            break;
        case KEY_UP:
            ++scroll_;
            break;
        case KEY_DOWN:
            if (scroll_ > 0) {
                --scroll_;
            }
            break;
        case KEY_HOME:
            scroll_ = maxScroll();
            break;
        case KEY_END:
            scroll_ = 0;
            break;
        case KEY_BACKSPACE:
        case 8:
        case 127:
            if (cursor_ > 0) {
                text_.erase(cursor_ - 1, 1);
                --cursor_;
            }
            break;
        case KEY_DC:
            if (cursor_ < text_.size()) {
                text_.erase(cursor_, 1);
            }
            break;
        case KEY_LEFT:
            if (cursor_ > 0) {
                --cursor_;
            }
            break;
        case KEY_RIGHT:
            if (cursor_ < text_.size()) {
                ++cursor_;
            }
            break;
        case 1:  // ^A
            cursor_ = 0;
            break;
        case 5:  // ^E
            cursor_ = text_.size();
            break;
        case 21:  // ^U
            text_.erase(0, cursor_);
            cursor_ = 0;
            break;
        case 10:
        case 13:
        case KEY_ENTER:
            submit();
            break;
        default:
            if (key >= 32 && key < 127 && text_.size() < kMaxInputLength) {
                text_.insert(cursor_, 1, static_cast<char>(key));
                ++cursor_;
            }
            break;
    }

    if (scroll_ > maxScroll()) {
        scroll_ = maxScroll();
    }
}

void Tui::submit() {
    const std::string line = trim(text_);
    if (line.empty()) {
        text_.clear();
        cursor_ = 0;
        return;
    }
    if (line.front() == '/') {
        text_.clear();
        cursor_ = 0;
        runCommand(line);
        return;
    }
    if (!signedIn_) {
        // Fast typists can beat the server's reply; hold their words rather
        // than silently dropping them, then flush once we are signed in.
        if (loginSent_) {
            pending_.push_back(line);
            text_.clear();
            cursor_ = 0;
            appendSystem("holding \"" + line + "\" until the server confirms your name");
            return;
        }
        text_.clear();
        cursor_ = 0;
        name_ = line;
        loginSent_ = true;
        status_ = "signing in";
        statusColor_ = kColorSystem;
        Connection* active = activeConnection_.load();
        if (!active->failed() && loginGen_ != transportGen_.load()) {
            loginGen_ = transportGen_.load();
            sendLogin();
        } else if (active->failed()) {
            appendSystem("server unreachable; signing in as soon as it is back");
            monitorCv_.notify_all();
        }
        return;
    }

    text_.clear();
    cursor_ = 0;
    deliver(line);
}

void Tui::runCommand(const std::string& command) {
    std::istringstream stream(command);
    std::string name;
    stream >> name;

    if (name == "/quit" || name == "/exit") {
        quit_ = true;
    } else if (name == "/clear") {
        entries_.clear();
        rows_.clear();
        scroll_ = 0;
    } else if (name == "/users") {
        if (users_.empty()) {
            appendSystem("nobody else is here yet");
            return;
        }
        std::string listing = "online:";
        for (const std::string& user : users_) {
            listing += " " + user;
            std::string host;
            std::uint16_t peerPort = 0;
            if (peers_.peerAddress(user, host, peerPort)) {
                listing += " (" + host + ":" + std::to_string(peerPort) + ")";
            }
            if (peers_.connectedTo(user)) {
                listing += " (direct)";
            } else if (useRelay_) {
                listing += " (relay)";
            }
        }
        appendSystem(listing);
    } else if (name == "/color") {
        std::string color;
        stream >> color;
        if (color.empty()) {
            const std::string who = name_.empty() ? "you" : name_;
            appendSystem("usage: /color <" + paletteList() + "|0-255>");
            appendSystem("right now " + who, pairFor(color_));
            return;
        }
        int customIndex = 0;
        const bool customColor = parseColorIndex(color, customIndex);
        if (customColor && (!has_colors() || COLORS < 256)) {
            appendSystem("custom colors need a 256-color terminal", kColorBad);
            return;
        }
        if (customColor && isReservedSystemColor(customIndex)) {
            appendSystem("color " + color + " is reserved for system messages", kColorBad);
            return;
        }
        if (!isValidColor(color)) {
            appendSystem("unknown color: " + color + " (try " + paletteList() + ", or 0-255)",
                         kColorBad);
            return;
        }
        color_ = color;
        appendSystem("you chose color " + color, pairFor(color_));
    } else if (name == "/help") {
        appendSystem("/help           show this list");
        appendSystem("/users          list everyone online");
        appendSystem("/color <shade>  use a candy shade or xterm color 0-255");
        appendSystem("/clear          clear the message pane");
        appendSystem("/quit, /exit    leave the chat");
    } else {
        appendSystem("unknown command: " + name, kColorBad);
    }
}

}  // namespace chat
