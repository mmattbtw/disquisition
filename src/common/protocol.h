#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace chat {

// This file defines the language spoken by the server, relay, and clients.
// Each program uses the same message types and the same frame layout. If one
// program encoded a message differently, the program on the other end would
// not know how to read it.
//
// A frame is one complete message sent over a TCP connection:
//
//     [payload size][message type][field length][field text]...
//
// The payload size and each field length use four bytes. The message type uses
// one byte. A Login message for "matt" contains three fields, so its payload
// can be pictured like this:
//
//     [Login][4][matt][1][0][0][]
//
// The actual lengths occupy four bytes even though the example shows them as
// ordinary numbers. Lengths use big-endian byte order, which puts the largest
// part of a number first. Network protocols use this order so computers with
// different processors agree on how to read a number.
//
// The server handles sign-in, peer discovery and message storage, but live
// chat does not pass through it. Peers exchange live chat frames directly, or
// a relay exchanges those frames for clients that cannot accept connections.
enum class MsgType : std::uint8_t {
    // Messages a client sends to the central server.
    Login = 1,         // [name, chat port, advertised host, voice SIP port (optional)]
    // Store saves a message that was already delivered live. This is what
    // makes the message appear when another client requests chat history.
    Store = 10,        // [timestamp, body, color]
    FetchHistory = 11, // []
    VoicePort = 19,    // client: [SIP port, 0 to leave]; server: [name, SIP port]
    VoiceAudio = 20,   // client: [16 kHz mono signed-16 PCM]; server: [sender, PCM]
    RelayProbe = 21,  // client to relay: [] health check without signing in
    RelayReady = 22,  // relay to client: [] server connection is available

    // Messages the central server sends to a client.
    LoginOk = 3,    // [assigned name, welcome text]
    Error = 4,      // [reason]
    History = 6,    // [timestamp, sender, body, color]
    System = 7,     // [text]
    Users = 8,      // [name]*
    HistoryEnd = 9, // []
    Peer = 12,      // [name, host, chat port, advertised (0|1), voice SIP port]
    PeerJoined = 13, // [name, host, chat port, advertised (0|1), voice SIP port]
    PeerLeft = 14,  // [name]

    // Messages sent over a direct peer connection.
    //
    // Hello introduces the peer that opened the connection. The sender field
    // says who called. The target field says who they meant to call. Relays
    // need the target because several users can share one relay port.
    Hello = 15,
    HelloOk = 17, // [target] confirms that the requested peer accepted the link
    // PeerChat includes the sender's name because a relayed client receives
    // every user's messages through one socket. The socket alone cannot tell
    // that client who wrote a message.
    PeerChat = 16, // [sender, timestamp, body, color]
    VoiceState = 18 // [sender, muted (0|1), deafened (0|1)]
};

// Rejecting larger frames prevents a broken or hostile client from making the
// receiver reserve an unreasonable amount of memory.
constexpr std::uint32_t kMaxFrameSize = 16 * 1024;

// Every program uses the same limits so a client cannot create a name or
// message that the server refuses to handle.
constexpr std::size_t kMaxNameLength = 20;
constexpr std::size_t kMaxBodyLength = 2000;

// Display colors shared by the server and every client. Named candy shades
// and custom xterm-256 indexes travel with each message as strings; clients
// map them onto ncurses color pairs.
constexpr const char* kColorNames[] = {
    "pink", "mint", "butter", "periwinkle", "lilac", "aqua", "peach"};
constexpr std::size_t kColorCount = sizeof(kColorNames) / sizeof(kColorNames[0]);

inline bool parseColorIndex(const std::string& color, int& index) {
    // A color index contains one to three decimal digits.
    if (color.empty() || color.size() > 3) {
        return false;
    }

    // Build the number one digit at a time. For "123", the value changes from
    // 0 to 1, then 12, then 123.
    int value = 0;
    for (const char character : color) {
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
    // The interface uses these colors for errors, notices, and other text that
    // should remain visually different from user messages.
    return index == 1 || index == 2 || index == 250;
}

inline bool isValidColor(const std::string& color) {
    // A color may be one of the friendly names above or an available numeric
    // xterm-256 color.
    for (std::size_t index = 0; index < kColorCount; ++index) {
        if (color == kColorNames[index]) {
            return true;
        }
    }
    int index = 0;
    return parseColorIndex(color, index) && !isReservedSystemColor(index);
}

struct Message {
    // type explains the purpose of the message. fields contains its data in
    // the order documented beside that message type above.
    MsgType type = MsgType::Error;
    std::vector<std::string> fields;
};

// Converts a Message object into the bytes sent through a socket.
std::string encode(const Message& message);

// TCP may deliver half a frame or several frames in one read. decode reports
// which of those cases it found so the caller knows whether to wait, use a
// message, or close a bad connection.
enum class DecodeStatus {
    Ok,          // `out` holds one message and it was removed from `buffer`
    Incomplete,  // `buffer` holds a partial frame, call again after more data
    Malformed    // `buffer` is unusable and the connection should be dropped
};

// Reads one frame from the front of buffer. A successful call removes only
// that frame, leaving any later frame in buffer for the next call.
DecodeStatus decode(std::string& buffer, Message& out);

// Returns a readable name for logs, such as "Login" or "PeerChat".
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
