#include "client/tui.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <exception>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <random>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "client/text_wrap.h"
#include "common/net.h"
#include "util/log.h"

namespace chat {
namespace {

constexpr std::size_t kMaxEntries = 1000;
constexpr std::size_t kEntriesDroppedWhenFull = 200;
constexpr std::size_t kMaxInputLength = 2000;
constexpr std::size_t kMaxSeen = 2000;
constexpr int kReconnectIntervalMs = 3000;
constexpr int kReconnectTimeoutMs = 3000;
constexpr int kInitialConnectTimeoutMs = 5000;
constexpr int kInputTimeoutMs = 80;

// ncurses color pairs.
constexpr int kColorOwn = 1;
constexpr int kColorOther = 2;
constexpr int kColorSystem = 3;
constexpr int kColorHeader = 4;
constexpr int kColorGood = 5;
constexpr int kColorBad = 6;
constexpr int kColorSelf = 7;
constexpr int kFirstPaletteColor = 20; // one pair per kColorNames entry

// Foregrounds for kColorNames, in the same order.
constexpr int kBasicPalette[] = {COLOR_MAGENTA, COLOR_CYAN, COLOR_YELLOW, COLOR_BLUE,
                                 COLOR_MAGENTA, COLOR_CYAN, COLOR_BLUE};
constexpr int kCandyPalette[] = {211, 121, 229, 111, 183, 159, 216}; // xterm-256
constexpr int kCandySystemColor = 250;
constexpr int kBrightWhite = 231;

constexpr int ctrl(char key) {
    return key & 0x1F;
}

std::string dedupeKey(const std::string& sender, const std::string& timestamp,
                      const std::string& body) {
    return sender + '\x1f' + timestamp + '\x1f' + body;
}

std::string formatTime(const std::string& epochSeconds) {
    const std::time_t stamp = std::strtoll(epochSeconds.c_str(), nullptr, 10);
    std::tm local{};
    if (localtime_r(&stamp, &local) == nullptr) {
        return "--:--";
    }
    char buffer[16] = {};
    std::strftime(buffer, sizeof(buffer), "%H:%M", &local);
    return buffer;
}

std::string randomColorName() {
    static std::mt19937 engine(std::random_device{}());
    std::uniform_int_distribution<std::size_t> pick(0, kColorCount - 1);
    return kColorNames[pick(engine)];
}

std::string paletteList() {
    std::string list;
    for (const char* name : kColorNames) {
        if (!list.empty()) {
            list += ", ";
        }
        list += name;
    }
    return list;
}

} // namespace

Tui::Tui() {
    statusColor_ = kColorSystem;
}

Tui::~Tui() {
    stop();
}

int Tui::run(const ClientOptions& options) {
    options_ = options;
    if (!startPeers()) {
        return 1;
    }
    connectUpstream();

    activeConnection_.store(&connection_);
    relayActive_.store(options_.useRelay);
    if (options_.useRelay) {
        peers_.setPassive(true);
    }
    name_ = trim(options_.name);
    relayRequestedName_ = name_;
    color_ = randomColorName();

    if (!start()) {
        std::fprintf(stderr, "cannot initialise terminal\n");
        return 1;
    }
    peers_.setMyAdvertised(!options_.advertiseHost.empty());
    layout();
    showGreeting();

    if (name_.empty()) {
        appendSystem("type a name and press enter to join");
    }
    else if (!connection_.failed()) {
        signIn();
    }

    monitorRunning_.store(true);
    std::thread monitor([this] { monitorServer(); });

    while (!quit_) {
        if (shutdownRequested()) {
            quit_ = true;
        }
        checkConnectionLost();
        drainServer();
        drainPeers();
        resumeAfterReconnect();

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

// ---------------------------------------------------------------------------
// Screen

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
        const bool candy = COLORS >= 256;
        init_pair(kColorOwn, COLOR_CYAN, -1);
        init_pair(kColorOther, COLOR_MAGENTA, -1);
        init_pair(kColorSystem, candy ? kCandySystemColor : COLOR_WHITE, -1);
        init_pair(kColorHeader, COLOR_BLACK, COLOR_CYAN);
        init_pair(kColorGood, COLOR_GREEN, -1);
        init_pair(kColorBad, COLOR_RED, -1);
        init_pair(kColorSelf, candy ? kBrightWhite : COLOR_WHITE, -1);
        for (std::size_t index = 0; index < kColorCount; ++index) {
            const int color = candy ? kCandyPalette[index] : kBasicPalette[index];
            init_pair(kFirstPaletteColor + static_cast<int>(index), color, -1);
        }
    }
    installShutdownHandlers();
    return true;
}

void Tui::stop() {
    for (WINDOW** window : {&messages_, &input_, &header_}) {
        if (*window != nullptr) {
            delwin(*window);
            *window = nullptr;
        }
    }
    if (stdscr != nullptr) {
        curs_set(1);
        endwin();
    }
}

void Tui::layout() {
    int rows = 0;
    int columns = 0;
    getmaxyx(stdscr, rows, columns);
    width_ = columns > 0 ? columns : 80;
    messageHeight_ = std::max(1, rows - 2);

    for (WINDOW* window : {messages_, input_, header_}) {
        if (window != nullptr) {
            delwin(window);
        }
    }
    header_ = newwin(1, width_, 0, 0);
    messages_ = newwin(messageHeight_, width_, 1, 0);
    input_ = newwin(1, width_, rows > 1 ? rows - 1 : 0, 0);
    if (input_ != nullptr) {
        keypad(input_, TRUE);
        wtimeout(input_, kInputTimeoutMs);
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

    const std::string right = std::to_string(users_.size()) + " online  " + status_ + " ";
    const int start = width_ - static_cast<int>(right.size()) - 1;
    if (start <= static_cast<int>(left.size())) {
        return;
    }
    if (has_colors()) {
        wattron(header_, COLOR_PAIR(statusColor_));
    }
    mvwaddstr(header_, 0, start, right.c_str());
    if (has_colors()) {
        wattroff(header_, COLOR_PAIR(statusColor_));
    }
}

void Tui::drawMessages() {
    werase(messages_);
    scroll_ = std::min(scroll_, maxScroll());

    const std::size_t total = rows_.size();
    const auto height = static_cast<std::size_t>(messageHeight_);
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

    // Scroll the input horizontally so the cursor stays visible.
    const auto available = static_cast<std::size_t>(std::max(1, width_ - labelLength));
    const std::size_t start = cursor_ >= available ? cursor_ - available + 1 : 0;
    const std::string visible = text_.substr(start, available);
    const int pair = pairFor(color_);
    if (has_colors()) {
        wattron(input_, COLOR_PAIR(pair));
    }
    mvwaddstr(input_, 0, labelLength, visible.c_str());
    if (has_colors()) {
        wattroff(input_, COLOR_PAIR(pair));
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
    const auto height = static_cast<std::size_t>(std::max(1, messageHeight_));
    return rows_.size() > height ? rows_.size() - height : 0;
}

// Palette names have fixed pairs; numeric xterm colors get a pair allocated
// the first time they are seen.
int Tui::pairFor(const std::string& color) const {
    const auto named = std::find(kColorNames.begin(), kColorNames.end(), color);
    if (named != kColorNames.end()) {
        return kFirstPaletteColor + static_cast<int>(named - kColorNames.begin());
    }

    int index = 0;
    if (!parseColorIndex(color, index) || isReservedSystemColor(index) || !has_colors() ||
        COLORS < 256 || index >= COLORS) {
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
    if (init_pair(static_cast<short>(pair), static_cast<short>(index), -1) == ERR) {
        return kColorOther;
    }
    customColorPairs_[color] = pair;
    return pair;
}

// ---------------------------------------------------------------------------
// Message pane

void Tui::append(const std::string& prefix, const std::string& body, int color) {
    entries_.push_back(Entry{prefix, body, color});
    if (entries_.size() > kMaxEntries) {
        entries_.erase(entries_.begin(),
                       entries_.begin() + static_cast<std::ptrdiff_t>(kEntriesDroppedWhenFull));
        rebuildRows();
        return;
    }

    const std::vector<std::string> wrapped = wrapText(prefix, body, width_);
    for (const std::string& line : wrapped) {
        rows_.push_back(Row{line, color});
    }
    // Keep the view still while the user is reading scrollback.
    if (scroll_ > 0) {
        scroll_ += wrapped.size();
    }
}

void Tui::appendSystem(const std::string& text) {
    appendSystem(text, kColorSystem);
}

void Tui::appendSystem(const std::string& text, int color) {
    append("", "* " + text, color);
}

void Tui::rebuildRows() {
    rows_.clear();
    for (const Entry& entry : entries_) {
        for (const std::string& line : wrapText(entry.prefix, entry.body, width_)) {
            rows_.push_back(Row{line, entry.color});
        }
    }
}

void Tui::showGreeting() {
    const bool relay = options_.useRelay;
    const std::string endpoint = relay
                                     ? options_.relayHost + ":" + std::to_string(options_.relayPort)
                                     : options_.host + ":" + std::to_string(options_.port);
    std::string greeting;
    if (connection_.failed()) {
        greeting = (relay ? "relay " : "server ") + endpoint + " is unreachable; retrying";
    }
    else if (relay) {
        greeting = "connected to relay " + endpoint;
    }
    else {
        greeting = "connected to " + endpoint + " (peer port " + std::to_string(peers_.port()) +
                   ")";
        if (!options_.advertiseHost.empty()) {
            greeting += ", advertising " + options_.advertiseHost;
        }
    }
    // A relay may rename us, so only direct connections announce the name.
    if (!name_.empty() && !relay) {
        greeting += " as " + name_;
    }
    appendSystem(greeting, kColorGood);
}

void Tui::showChat(const std::string& sender, std::int64_t timestamp, const std::string& body,
                   const std::string& color) {
    const std::string stamp = std::to_string(timestamp);
    if (remember(sender, stamp, body)) {
        append(formatTime(stamp) + " " + sender + ": ", body, pairFor(color));
        recentMessages_.add({timestamp, sender, body, color});
    }
}

void Tui::showHistory(const Message& message) {
    if (!historyOpen_) {
        historyOpen_ = true;
        appendSystem("recent messages");
    }
    if (message.fields.size() < 4 || !isValidColor(message.fields[3])) {
        return;
    }
    const std::string& timestamp = message.fields[0];
    const std::string& sender = message.fields[1];
    const std::string& body = message.fields[2];
    if (remember(sender, timestamp, body)) {
        append(formatTime(timestamp) + " " + sender + ": ", body, pairFor(message.fields[3]));
        recentMessages_.add({std::strtoll(timestamp.c_str(), nullptr, 10), sender, body,
                             message.fields[3]});
    }
}

bool Tui::remember(const std::string& sender, const std::string& timestamp,
                   const std::string& body) {
    if (seen_.size() > kMaxSeen) {
        seen_.clear();
    }
    return seen_.insert(dedupeKey(sender, timestamp, body)).second;
}

// ---------------------------------------------------------------------------
// Session

// The listener must be up before Login announces its port. A relay-only
// client never listens: the relay accepts peers on its behalf.
bool Tui::startPeers() {
    if (options_.useRelay && !options_.leakMyIp) {
        return true;
    }
    std::string error;
    if (!peers_.start(options_.peerPort, error)) {
        LOG_ERR("cannot listen for peers on port {}: {}", options_.peerPort, error);
        std::fprintf(stderr, "cannot listen for peers on port %u: %s\n", options_.peerPort,
                     error.c_str());
        return false;
    }
    return true;
}

// On failure the monitor thread keeps retrying in the background.
void Tui::connectUpstream() {
    const std::string& host = options_.useRelay ? options_.relayHost : options_.host;
    const std::uint16_t port = options_.useRelay ? options_.relayPort : options_.port;
    std::string error;
    if (connection_.connectTo(host, port, kInitialConnectTimeoutMs, error)) {
        connection_.startReader();
        return;
    }
    LOG_WARN("cannot connect to {}:{}: {}; retrying", host, port, error);
    std::fprintf(stderr, "cannot connect to %s:%u: %s; retrying in the background\n", host.c_str(),
                 port, error.c_str());
}

void Tui::setStatus(const std::string& text, int color) {
    status_ = text;
    statusColor_ = color;
}

void Tui::signIn() {
    loginGen_ = transportGen_.load();
    loginSent_ = true;
    setStatus("signing in", kColorSystem);
    sendLogin();
}

void Tui::sendLogin() {
    const bool throughRelay = relayActive_.load();
    if (throughRelay && relayRequestedName_.empty()) {
        relayRequestedName_ = name_;
    }
    // The relay listens for peers on our behalf, so it supplies the address.
    const std::string& name = throughRelay ? relayRequestedName_ : name_;
    const std::string advertise = throughRelay ? "" : options_.advertiseHost;
    activeConnection_.load()->send(
        Message{MsgType::Login, {name, std::to_string(peers_.port()), advertise}});
}

void Tui::onSignedIn(const Message& message) {
    signedIn_ = true;
    loginSent_ = false;
    if (!message.fields.empty()) {
        name_ = message.fields[0];
    }
    peers_.setMyName(name_);
    serverReady_ = true;
    serverLost_ = false;
    setStatus("online", kColorGood);
    LOG_INFO("signed in as {}", name_);
    appendSystem("you are signed in as " + name_, kColorGood);

    if (!pending_.empty()) {
        for (const std::string& line : pending_) {
            deliver(line);
        }
        appendSystem("sent " + std::to_string(pending_.size()) +
                     " message(s) typed while signing in");
        pending_.clear();
    }
}

void Tui::deliver(const std::string& line) {
    const std::string body = sanitizeBody(line);
    if (body.empty()) {
        return;
    }
    const std::int64_t timestamp = std::time(nullptr);
    const std::string stamp = std::to_string(timestamp);
    remember(name_, stamp, body);
    append(formatTime(stamp) + " you: ", body, kColorSelf);
    recentMessages_.add({timestamp, name_, body, color_});

    Connection* active = activeConnection_.load();
    if (relayActive_.load()) {
        active->send(Message{MsgType::PeerChat, {name_, stamp, body, color_}});
    }
    else {
        peers_.sendChat(timestamp, body, color_);
    }
}

// Losing the server costs discovery only: peer links keep
// working and the monitor thread keeps reconnecting.
void Tui::checkConnectionLost() {
    Connection* active = activeConnection_.load();
    if (!active->failed() || serverLost_) {
        return;
    }
    // Frames queued behind the disconnect are stale; the next login resyncs.
    Message stale;
    while (active->poll(stale)) {
    }
    serverLost_ = true;
    serverReady_ = false;
    setStatus("server offline", kColorBad);
    LOG_WARN("{} connection lost", relayActive_.load() ? "relay" : "server");
    appendSystem(relayActive_.load() ? "relay connection lost; reconnecting"
                                     : "server connection lost; peer-to-peer chat still works",
                 kColorBad);
    dirty_ = true;
}

void Tui::resumeAfterReconnect() {
    if (!serverBack_.exchange(false) || activeConnection_.load()->failed()) {
        return;
    }
    LOG_INFO("{} connection restored", relayActive_.load() ? "relay" : "server");
    serverLost_ = false;
    if (transportChanged_.exchange(false)) {
        appendSystem(relayActive_.load() ? "relay connection restored"
                                         : "relay unavailable; connected directly to " +
                                               options_.host + ":" + std::to_string(options_.port),
                     kColorGood);
    }
    if (name_.empty()) {
        setStatus("connecting", kColorSystem);
    }
    else if (loginGen_ != transportGen_.load()) {
        signIn();
    }
    dirty_ = true;
}

void Tui::drainServer() {
    Message message;
    while (activeConnection_.load()->poll(message)) {
        dirty_ = true;
        switch (message.type) {
            case MsgType::LoginOk:
                onSignedIn(message);
                break;
            case MsgType::History:
                showHistory(message);
                break;
            case MsgType::HistoryEnd:
                historyOpen_ = false;
                break;
            case MsgType::Peer:
            case MsgType::PeerJoined: {
                PeerAddress peer;
                if (!parsePeerAddress(message, peer)) {
                    break;
                }
                peers_.addPeer(peer.name, peer.host, peer.port, peer.advertised);
                LOG_INFO("{} is online at {}:{}", peer.name, peer.host, peer.port);
                const std::string address = peer.host + ":" + std::to_string(peer.port);
                appendSystem(message.type == MsgType::Peer
                                 ? peer.name + " is online at " + address
                                 : peer.name + " joined (" + address + ")");
                break;
            }
            case MsgType::PeerLeft:
                if (!message.fields.empty()) {
                    peers_.removePeer(message.fields[0]);
                    LOG_INFO("{} left", message.fields[0]);
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
            case MsgType::PeerChat: {
                // Only a relay forwards chat on this connection.
                ChatPayload payload;
                if (relayActive_.load() && parsePeerChat(message, payload)) {
                    showChat(payload.sender, payload.timestamp, payload.body, payload.color);
                }
                break;
            }
            case MsgType::Error:
                LOG_WARN("server error: {}",
                         message.fields.empty() ? "unknown" : message.fields[0]);
                appendSystem(message.fields.empty() ? "server error" : message.fields[0],
                             kColorBad);
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
                showChat(event.name, event.timestamp, event.body, event.color);
                break;
            case PeerNetwork::Event::Kind::Join:
                appendSystem("direct link to " + event.name + " is up", kColorGood);
                break;
            case PeerNetwork::Event::Kind::Leave:
                appendSystem("direct link to " + event.name + " is down", kColorBad);
                break;
            case PeerNetwork::Event::Kind::Note:
                LOG_WARN("{}", event.body);
                appendSystem(event.body, kColorBad);
                break;
        }
    }
}

// ---------------------------------------------------------------------------
// Input

void Tui::handleKey(int key) {
    const auto page = static_cast<std::size_t>(messageHeight_);
    switch (key) {
        case ctrl('C'):
        case ctrl('D'):
            quit_ = true;
            return;
        case ctrl('L'):
            clearok(curscr, TRUE);
            return;
        case KEY_RESIZE:
            layout();
            return;

        case KEY_PPAGE:
            scroll_ += std::max<std::size_t>(1, page - 1);
            break;
        case KEY_NPAGE:
            scroll_ = scroll_ > page ? scroll_ - page : 0;
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
        case ctrl('H'):
        case 127:
            if (cursor_ > 0) {
                text_.erase(--cursor_, 1);
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
        case ctrl('A'):
            cursor_ = 0;
            break;
        case ctrl('E'):
            cursor_ = text_.size();
            break;
        case ctrl('U'):
            text_.erase(0, cursor_);
            cursor_ = 0;
            break;
        case '\n':
        case '\r':
        case KEY_ENTER:
            submit();
            break;
        default:
            if (key >= ' ' && key < 127 && text_.size() < kMaxInputLength) {
                text_.insert(cursor_++, 1, static_cast<char>(key));
            }
            break;
    }
    scroll_ = std::min(scroll_, maxScroll());
}

void Tui::clearInput() {
    text_.clear();
    cursor_ = 0;
}

void Tui::submit() {
    const std::string line = trim(text_);
    clearInput();
    if (line.empty()) {
        return;
    }
    if (line.front() == '/') {
        runCommand(line);
        return;
    }
    if (signedIn_) {
        deliver(line);
        return;
    }

    // Hold anything typed while the login is in flight and send it once the
    // server confirms the name.
    if (loginSent_) {
        pending_.push_back(line);
        appendSystem("holding \"" + line + "\" until the server confirms your name");
        return;
    }

    // Before sign-in, the first line is the user's name.
    name_ = line;
    loginSent_ = true;
    setStatus("signing in", kColorSystem);
    Connection* active = activeConnection_.load();
    if (active->failed()) {
        appendSystem("server unreachable; signing in as soon as it is back");
        monitorCv_.notify_all();
    }
    else if (loginGen_ != transportGen_.load()) {
        loginGen_ = transportGen_.load();
        sendLogin();
    }
}

void Tui::runCommand(const std::string& command) {
    std::istringstream stream(command);
    std::string name;
    std::string argument;
    stream >> name >> argument;

    if (name == "/quit" || name == "/exit") {
        quit_ = true;
    }
    else if (name == "/clear") {
        entries_.clear();
        rows_.clear();
        recentMessages_.clear();
        scroll_ = 0;
    }
    else if (name == "/save") {
        const std::string path = trim(command.substr(name.size()));
        if (path.empty()) {
            appendSystem("usage: /save <path.db>", kColorBad);
        }
        else if (recentMessages_.size() == 0) {
            appendSystem("no recent chat messages to save", kColorBad);
        }
        else {
            try {
                recentMessages_.save(path);
                appendSystem("saved " + std::to_string(recentMessages_.size()) +
                             " chat messages to " + path, kColorGood);
            }
            catch (const std::exception& error) {
                appendSystem(error.what(), kColorBad);
            }
        }
    }
    else if (name == "/users") {
        listUsers();
    }
    else if (name == "/color") {
        changeColor(argument);
    }
    else if (name == "/help") {
        appendSystem("/help           show this list");
        appendSystem("/users          list everyone online");
        appendSystem("/color <shade>  use a candy shade or xterm color 0-255");
        appendSystem("/clear          clear the message pane");
        appendSystem("/save <path.db> save recent chat messages to SQLite");
        appendSystem("/quit, /exit    leave the chat");
    }
    else {
        appendSystem("unknown command: " + name, kColorBad);
    }
}

void Tui::listUsers() {
    if (users_.empty()) {
        appendSystem("nobody else is here yet");
        return;
    }
    std::string listing = "online:";
    for (const std::string& user : users_) {
        listing += " " + user;
        std::string host;
        std::uint16_t port = 0;
        if (peers_.peerAddress(user, host, port)) {
            listing += " (" + host + ":" + std::to_string(port) + ")";
        }
        if (peers_.connectedTo(user)) {
            listing += " (direct)";
        }
        else if (options_.useRelay) {
            listing += " (relay)";
        }
    }
    appendSystem(listing);
}

void Tui::changeColor(const std::string& color) {
    if (color.empty()) {
        appendSystem("usage: /color <" + paletteList() + "|0-255>");
        appendSystem("right now " + (name_.empty() ? std::string("you") : name_), pairFor(color_));
        return;
    }
    int index = 0;
    const bool numeric = parseColorIndex(color, index);
    if (numeric && (!has_colors() || COLORS < 256)) {
        appendSystem("custom colors need a 256-color terminal", kColorBad);
        return;
    }
    if (numeric && isReservedSystemColor(index)) {
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
}

// ---------------------------------------------------------------------------
// Monitor thread

void Tui::monitorServer() {
    const bool canBypassRelay = options_.useRelay && options_.leakMyIp;
    for (;;) {
        {
            std::unique_lock<std::mutex> lock(monitorMutex_);
            monitorCv_.wait_for(lock, std::chrono::milliseconds(kReconnectIntervalMs));
            if (!monitorRunning_.load()) {
                return;
            }
        }
        if (activeConnection_.load()->failed()) {
            reconnect();
        }
        else if (canBypassRelay && !relayActive_.load()) {
            returnToRelay();
        }
    }
}

void Tui::reconnect() {
    Connection* active = activeConnection_.load();
    active->stop();
    std::string error;

    const bool bypassRelay = options_.useRelay && options_.leakMyIp && relayActive_.load();
    if (!bypassRelay) {
        const std::string& host = relayActive_.load() ? options_.relayHost : options_.host;
        const std::uint16_t port = relayActive_.load() ? options_.relayPort : options_.port;
        if (active->connectTo(host, port, kReconnectTimeoutMs, error)) {
            active->startReader();
            markTransportUp();
        }
        return;
    }

    // The relay is down and --leak-my-ip allows going direct until it returns.
    if (directConnection_.connectTo(options_.host, options_.port, kReconnectTimeoutMs, error)) {
        directConnection_.startReader();
        useTransport(&directConnection_, /*throughRelay=*/false);
        markTransportUp();
    }
    else if (connection_.connectTo(options_.relayHost, options_.relayPort, kReconnectTimeoutMs,
                                   error)) {
        connection_.startReader();
        markTransportUp();
    }
}

void Tui::returnToRelay() {
    connection_.stop();
    std::string error;
    if (!connection_.connectTo(options_.relayHost, options_.relayPort, kReconnectTimeoutMs,
                               error)) {
        return;
    }
    connection_.startReader();
    useTransport(&connection_, /*throughRelay=*/true);
    directConnection_.stop();
    markTransportUp();
}

void Tui::useTransport(Connection* connection, bool throughRelay) {
    activeConnection_.store(connection);
    relayActive_.store(throughRelay);
    peers_.setPassive(throughRelay);
    transportChanged_.store(true);
}

void Tui::markTransportUp() {
    transportGen_.fetch_add(1);
    serverBack_.store(true);
}

} // namespace chat
