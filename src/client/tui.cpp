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

// Colour pair numbers registered in Tui::start().
constexpr int kColourOwn = 1;
constexpr int kColourOther = 2;
constexpr int kColourSystem = 3;
constexpr int kColourHeader = 4;
constexpr int kColourGood = 5;
constexpr int kColourBad = 6;
// Your own sent lines, drawn bold-white so they never match a peer's colour.
constexpr int kColourOwnMsg = 7;
// First colour pair number used for per-user palette colours.
constexpr int kFirstUserColour = 20;

// Maps a palette name to the ncurses foreground colour value used to build
// that user's pair. Order must line up with kColorNames.
constexpr int kUserColourValues[] = {
    COLOR_RED, COLOR_GREEN, COLOR_YELLOW, COLOR_BLUE, COLOR_MAGENTA, COLOR_CYAN, COLOR_WHITE};

volatile std::sig_atomic_t gInterrupted = 0;

// A stable hash of a name so every client agrees on a user's default colour
// without needing to ask the server or a peer.
std::string hashColour(const std::string& name) {
    std::uint64_t hash = 1469598103934665603ULL;  // FNV-1a offset basis
    for (const unsigned char character : name) {
        hash ^= character;
        hash *= 1099511628211ULL;
    }
    return kColorNames[hash % kColorCount];
}

// A friendly default handle for someone who did not pass --name.
std::string randomName() {
    constexpr const char* kAdjectives[] = {"Brave",   "Cosmic", "Crimson", "Dapper",
                                           "Electric", "Golden", "Jolly",   "Midnight",
                                           "Neon",    "Nimble", "Quiet",   "Solar",
                                           "Swift",   "Velvet", "Witty",   "Zesty"};
    constexpr const char* kNouns[] = {"Badger", "Breeze", "Comet",  "Dolphin", "Echo",
                                      "Falcon", "Galaxy", "Lynx",   "Moose",   "Nimbus",
                                      "Nova",   "Otter",  "Panda",  "Raven",   "Tiger",
                                      "Wombat"};
    static thread_local std::mt19937 generator(
        static_cast<std::uint32_t>(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::uniform_int_distribution<std::size_t> adjective(0,
                                                         sizeof(kAdjectives) / sizeof(*kAdjectives) - 1);
    std::uniform_int_distribution<std::size_t> noun(0, sizeof(kNouns) / sizeof(*kNouns) - 1);
    return std::string(kAdjectives[adjective(generator)]) + "_" + kNouns[noun(generator)];
}

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
        init_pair(kColourOwn, COLOR_CYAN, -1);
        init_pair(kColourOther, COLOR_MAGENTA, -1);
        init_pair(kColourSystem, COLOR_YELLOW, -1);
        init_pair(kColourHeader, COLOR_BLACK, COLOR_CYAN);
        init_pair(kColourGood, COLOR_GREEN, -1);
        init_pair(kColourBad, COLOR_RED, -1);
        init_pair(kColourOwnMsg, COLOR_WHITE, -1);
        for (std::size_t index = 0; index < kColorCount; ++index) {
            init_pair(kFirstUserColour + static_cast<int>(index), kUserColourValues[index], -1);
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
             const std::string& advertiseHost) {
    host_ = host;
    port_ = port;
    advertiseHost_ = advertiseHost;
    name_ = trim(initialName);
    if (name_.empty()) {
        name_ = randomName();
    }

    if (!start()) {
        std::fprintf(stderr, "cannot initialise terminal\n");
        return 1;
    }

    peers_.setMyAdvertised(!advertiseHost_.empty());

    layout();

    std::string greeting;
    if (connection_.failed()) {
        greeting = "server " + host_ + ":" + std::to_string(port_) + " is unreachable; retrying";
    } else {
        greeting = "connected to " + host_ + ":" + std::to_string(port_) + " (peer port " +
                   std::to_string(peers_.port()) + ")";
        if (!advertiseHost_.empty()) {
            greeting += ", advertising " + advertiseHost_;
        }
    }
    if (!name_.empty()) {
        greeting += " as " + name_;
    }
    appendSystem(greeting, kColourGood);

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
        if (connection_.failed() && !serverLost_) {
            Message stale;
            while (connection_.poll(stale)) {
            }
            serverLost_ = true;
            serverReady_ = false;
            status_ = "server offline";
            statusColour_ = kColourBad;
            appendSystem("server connection lost; peer-to-peer chat still works", kColourBad);
            dirty_ = true;
        }

        drainIncoming();
        drainPeers();

        // The monitor re-established the transport; sign in on it from here,
        // where all the UI state lives. The generation check sends exactly
        // one Login per transport no matter which thread observes it first.
        if (serverBack_.exchange(false) && !connection_.failed()) {
            serverLost_ = false;
            if (!name_.empty() && loginGen_ != transportGen_.load()) {
                loginGen_ = transportGen_.load();
                loginSent_ = true;
                status_ = "signing in";
                statusColour_ = kColourSystem;
                sendLogin();
            } else if (name_.empty()) {
                status_ = "connecting";
                statusColour_ = kColourSystem;
            }
            dirty_ = true;
        }

        // Fetch recent messages only once the mesh has settled, so nothing a
        // reachable peer already stored can slip between the fetch and the
        // live stream. Anything arriving through both is deduplicated.
        if (historyPending_ && serverReady_ && peers_.settled()) {
            historyPending_ = false;
            connection_.send(Message {MsgType::FetchHistory, {}});
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
    return connection_.failed() && !signedIn_ ? 1 : 0;
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
        if (connection_.failed()) {
            connection_.stop();
            std::string error;
            if (connection_.connectTo(host_, port_, kReconnectTimeoutMs, error)) {
                connection_.startReader();
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
        wbkgd(header_, COLOR_PAIR(kColourHeader));
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
            wattron(header_, COLOR_PAIR(statusColour_));
        }
        mvwaddstr(header_, 0, start, right.c_str());
        if (has_colors()) {
            wattroff(header_, COLOR_PAIR(statusColour_));
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
            if (row.colour == kColourOwnMsg) {
                wattron(messages_, A_BOLD);
            }
            wattron(messages_, COLOR_PAIR(row.colour));
        }
        mvwaddnstr(messages_, y, 0, row.text.c_str(), width_);
        if (has_colors()) {
            wattroff(messages_, COLOR_PAIR(row.colour));
            if (row.colour == kColourOwnMsg) {
                wattroff(messages_, A_BOLD);
            }
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
        wattron(input_, COLOR_PAIR(kColourOwn));
    }
    mvwaddstr(input_, 0, labelLength, visible.c_str());
    if (has_colors()) {
        wattroff(input_, COLOR_PAIR(kColourOwn));
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

void Tui::append(const std::string& prefix, const std::string& body, int colour) {
    entries_.push_back(Entry {prefix, body, colour});

    if (entries_.size() > kMaxEntries) {
        entries_.erase(entries_.begin(), entries_.begin() + static_cast<std::ptrdiff_t>(200));
        rebuildRows();
        return;
    }

    const std::vector<std::string> wrapped = wrapText(prefix, body, width_);
    const std::size_t added = wrapped.size();
    for (const std::string& line : wrapped) {
        rows_.push_back(Row {line, colour});
    }
    // Keep the viewport anchored when the user is reading scrollback.
    if (scroll_ > 0) {
        scroll_ += added;
    }
}

void Tui::appendSystem(const std::string& text, int colour) {
    append("", "* " + text, colour);
}

int Tui::pairFor(const std::string& colour) const {
    for (std::size_t index = 0; index < kColorCount; ++index) {
        if (colour == kColorNames[index]) {
            return kFirstUserColour + static_cast<int>(index);
        }
    }
    return kColourOther;
}

int Tui::colourFor(const std::string& sender) const {
    const auto it = colours_.find(sender);
    if (it != colours_.end()) {
        return pairFor(it->second);
    }
    return pairFor(hashColour(sender));
}

void Tui::rebuildRows() {
    rows_.clear();
    for (const Entry& entry : entries_) {
        for (const std::string& line : wrapText(entry.prefix, entry.body, width_)) {
            rows_.push_back(Row {line, entry.colour});
        }
    }
}

void Tui::drainIncoming() {
    Message message;
    while (connection_.poll(message)) {
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
                statusColour_ = kColourGood;
                appendSystem("you are signed in as " + name_, kColourGood);
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
                if (message.fields.size() < 3) {
                    break;
                }
                if (remember(message.fields[1], message.fields[0], message.fields[2])) {
                    append(formatTime(message.fields[0]) + " " + message.fields[1] + ": ",
                           message.fields[2], colourFor(message.fields[1]));
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
            case MsgType::Color:
                if (message.fields.size() >= 2 && isValidColor(message.fields[1])) {
                    const std::string& sender = message.fields[0];
                    if (colours_[sender] != message.fields[1]) {
                        colours_[sender] = message.fields[1];
                        if (sender != name_) {
                            appendSystem(sender + " chose color " + message.fields[1],
                                         colourFor(sender));
                        }
                    }
                }
                break;
            case MsgType::Error:
                appendSystem(message.fields.empty() ? "server error" : message.fields[0], kColourBad);
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
                           event.body, colourFor(event.name));
                }
                break;
            case PeerNetwork::Event::Kind::Join:
                appendSystem("direct link to " + event.name + " is up", kColourGood);
                break;
            case PeerNetwork::Event::Kind::Leave:
                appendSystem("direct link to " + event.name + " is down", kColourBad);
                break;
            case PeerNetwork::Event::Kind::Color:
                if (isValidColor(event.body) && colours_[event.name] != event.body) {
                    colours_[event.name] = event.body;
                    if (event.name != name_) {
                        appendSystem(event.name + " chose color " + event.body, colourFor(event.name));
                    }
                }
                break;
            case PeerNetwork::Event::Kind::Note:
                appendSystem(event.body, kColourBad);
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
    connection_.send(Message {MsgType::Login,
                              {name_, std::to_string(peers_.port()), advertiseHost_}});
}

void Tui::deliver(const std::string& line) {
    const std::string body = sanitizeBody(line);
    if (body.empty()) {
        return;
    }
    const std::int64_t timestamp = nowSeconds();
    const std::string stamp = std::to_string(timestamp);
    remember(name_, stamp, body);
    append(formatTime(stamp) + " you: ", body, kColourOwnMsg);
    peers_.sendChat(timestamp, body);
    if (serverReady_ && !connection_.failed()) {
        connection_.send(Message {MsgType::Store, {stamp, body}});
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
        statusColour_ = kColourSystem;
        if (!connection_.failed() && loginGen_ != transportGen_.load()) {
            loginGen_ = transportGen_.load();
            sendLogin();
        } else if (connection_.failed()) {
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
            }
        }
        appendSystem(listing);
    } else if (name == "/color") {
        std::string colour;
        stream >> colour;
        if (colour.empty()) {
            appendSystem("usage: /color <red|green|yellow|blue|magenta|cyan|white>");
            return;
        }
        if (!isValidColor(colour)) {
            appendSystem("unknown color: " + colour +
                             " (try red, green, yellow, blue, magenta, cyan, white)",
                         kColourBad);
            return;
        }
        colours_[name_] = colour;
        peers_.sendColor(colour);
        if (serverReady_ && !connection_.failed()) {
            connection_.send(Message {MsgType::SetColor, {colour}});
        }
        appendSystem("you chose color " + colour, colourFor(name_));
    } else if (name == "/help") {
        appendSystem("/help           show this list");
        appendSystem("/users          list everyone online");
        appendSystem("/color <name>   set your own display color");
        appendSystem("/clear          clear the message pane");
        appendSystem("/quit, /exit    leave the chat");
    } else {
        appendSystem("unknown command: " + name, kColourBad);
    }
}

}  // namespace chat
