#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace chat {

// Every message travels as: [uint32 payload size][uint8 type][field]*
// where each field is [uint32 length][bytes]. Lengths are big endian and the
// payload size never includes the four prefix bytes.
//
// The server handles sign-in, peer discovery and message storage, but live
// chat never passes through it: peers open raw TCP connections to each other
// and exchange frames directly.
enum class MsgType : std::uint8_t {
    // client -> server
    Login = 1,         // [name, peer port, advertised host (may be empty)]
    Store = 10,        // [timestamp, body, colour] archive a message already sent p2p
    FetchHistory = 11, // []

    // server -> client
    LoginOk = 3,    // [assigned name, welcome text]
    Error = 4,      // [reason]
    History = 6,    // [timestamp, sender, body, colour]
    System = 7,     // [text]
    Users = 8,      // [name]*
    HistoryEnd = 9, // []
    Peer = 12,      // [name, host, port, advertised (0|1)] one roster entry
    PeerJoined = 13, // [name, host, port, advertised (0|1)]
    PeerLeft = 14,  // [name]

    // peer -> peer
    // [sender, target] sent once by whichever side dialled. The receiver
    // learns the remote from `sender`; `target` names the peer being dialled,
    // which a relay uses to route a connection to the right hosted user when
    // many users share one public port.
    Hello = 15,
    HelloOk = 17, // [target] confirms that the requested peer accepted the link
    // Chat frames carry the sender's name so that anything reading
    // them without a per-peer connection can still attribute them. That is the
    // case for a client that reaches the mesh through a relay: every peer
    // arrives over the one relay socket, so the connection cannot identify the
    // sender the way a direct link can.
    PeerChat = 16  // [sender, timestamp, body, colour]
};

// Hard cap on a single frame so a hostile client cannot make us allocate.
constexpr std::uint32_t kMaxFrameSize = 16 * 1024;

// Limits shared by the server and every peer so both ends agree on them.
constexpr std::size_t kMaxNameLength = 20;
constexpr std::size_t kMaxBodyLength = 2000;

// Display colours shared by the server and every client. Named candy shades
// and custom xterm-256 indexes travel with each message as strings; clients
// map them onto ncurses colour pairs.
constexpr const char* kColorNames[] = {
    "pink", "mint", "butter", "periwinkle", "lilac", "aqua", "peach"};
constexpr std::size_t kColorCount = sizeof(kColorNames) / sizeof(kColorNames[0]);

// Keep accepting the old command names as aliases. Each renders as the candy
// shade in the same position.
constexpr const char* kLegacyColorNames[] = {
    "red", "green", "yellow", "blue", "magenta", "cyan", "white"};

inline bool parseColorIndex(const std::string& colour, int& index) {
    if (colour.empty() || colour.size() > 3) {
        return false;
    }
    int value = 0;
    for (const char character : colour) {
        if (character < '0' || character > '9') {
            return false;
        }
        value = value * 10 + (character - '0');
    }
    if (value > 255) {
        return false;
    }
    index = value;
    return true;
}

inline bool isReservedSystemColor(int index) {
    return index == 1 || index == 2 || index == 250;
}

inline bool isValidColor(const std::string& colour) {
    for (std::size_t index = 0; index < kColorCount; ++index) {
        if (colour == kColorNames[index] || colour == kLegacyColorNames[index]) {
            return true;
        }
    }
    int index = 0;
    return parseColorIndex(colour, index) && !isReservedSystemColor(index);
}

struct Message {
    MsgType type = MsgType::Error;
    std::vector<std::string> fields;
};

// Serializes a message into a complete, length prefixed frame.
std::string encode(const Message& message);

enum class DecodeStatus {
    Ok,          // `out` holds one message and it was removed from `buffer`
    Incomplete,  // `buffer` holds a partial frame, call again after more data
    Malformed    // `buffer` is unusable and the connection should be dropped
};

// Pulls a single frame off the front of `buffer`.
DecodeStatus decode(std::string& buffer, Message& out);

const char* typeName(MsgType type);

// Name rules: surrounding whitespace trimmed, spaces become underscores,
// control bytes become '?', capped at kMaxNameLength.
std::string sanitizeName(const std::string& name);

// Body rules: tabs and control bytes become spaces, trimmed and capped at
// kMaxBodyLength.
std::string sanitizeBody(const std::string& body);

// Strict base-10 parse (optional leading +/-). False on empty input, junk or
// more than 18 digits so the value can never overflow int64.
bool parseInt64(const std::string& text, std::int64_t& out);

// Strict decimal port parse. Zero is accepted only for listeners that ask the
// OS to choose an ephemeral port.
bool parsePort(const std::string& text, std::uint16_t& out, bool allowZero);

}  // namespace chat
