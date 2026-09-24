#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

// Wire protocol shared by the server, relay and clients.
//
// Every message travels as one length-prefixed frame:
//
//     [u32 payload size][u8 type]([u32 field length][field bytes])*
//
// All integers are big-endian. The server handles sign-in, discovery and
// history; live chat travels directly between peers (or through a relay) and
// never passes through the server.
namespace chat {

enum class MsgType : std::uint8_t {
    // Client -> server
    Login = 1,         // [name, peer port, advertised host (may be empty)]
    Store = 10,        // [timestamp, body, color]  a copy of chat already sent live
    FetchHistory = 11, // []
    VoicePort = 19,    // client: [SIP port, 0 to leave]; server: [name, SIP port]
    VoiceAudio = 20,   // client: [16 kHz mono signed-16 PCM]; server: [sender, PCM]

    // Server -> client
    LoginOk = 3,     // [assigned name, welcome text]
    Error = 4,       // [reason]
    History = 6,     // [timestamp, sender, body, color]
    System = 7,      // [text]
    Users = 8,       // [name]*
    HistoryEnd = 9,  // []
    Peer = 12,       // [name, host, port, advertised 0|1, voice SIP port]
    PeerJoined = 13, // [name, host, port, advertised 0|1, voice SIP port]
    PeerLeft = 14,   // [name]

    // Peer <-> peer. A relay hosts several users on one port, so Hello names
    // both the caller and the user being called, and PeerChat names its sender.
    Hello = 15,    // [sender, target]
    HelloOk = 17,  // [target]
    PeerChat = 16, // [sender, timestamp, body, color]
    VoiceState = 18, // [sender, muted 0|1, deafened 0|1]
    RelayProbe = 21, // client to relay: [] health check without signing in
    RelayReady = 22, // relay to client: [] server connection is available
};

struct Message {
    MsgType type = MsgType::Error;
    std::vector<std::string> fields;
};

constexpr std::uint32_t kMaxFrameSize = 16 * 1024;
constexpr std::size_t kMaxNameLength = 20;
constexpr std::size_t kMaxBodyLength = 2000;

// Messages carry their color as a string: one of these names, or an
// xterm-256 index such as "208".
constexpr std::array<const char*, 7> kColorNames = {"pink",  "mint", "butter", "periwinkle",
                                                    "lilac", "aqua", "peach"};
constexpr std::size_t kColorCount = kColorNames.size();

std::string encode(const Message& message);

enum class DecodeStatus {
    Ok,         // `out` holds the first frame, which was removed from `buffer`
    Incomplete, // wait for more bytes
    Malformed,  // the stream is corrupt; drop the connection
};

// Decodes the first frame in `buffer`, leaving any later frames in place.
DecodeStatus decode(std::string& buffer, Message& out);

// Parses "0" to "255".
bool parseColorIndex(const std::string& color, int& index);
// Indexes the terminal client keeps for its own notices.
bool isReservedSystemColor(int index);
bool isValidColor(const std::string& color);

// Trims, turns spaces into underscores and control bytes into '?', and caps
// the length at kMaxNameLength.
std::string sanitizeName(const std::string& name);
// Turns tabs and control bytes into spaces, trims, and caps the length at
// kMaxBodyLength.
std::string sanitizeBody(const std::string& body);
std::string trim(const std::string& text);

// Strict decimal parsers: no whitespace, no trailing junk, no overflow.
bool parseInt64(const std::string& text, std::int64_t& out);
bool parsePort(const std::string& text, std::uint16_t& out, bool allowZero);

struct ChatPayload {
    std::string sender; // as written by the sender; not verified
    std::int64_t timestamp = 0;
    std::string body; // sanitized
    std::string color;
};

// Validates a PeerChat frame: positive timestamp, non-empty body, valid color.
bool parsePeerChat(const Message& message, ChatPayload& out);

struct PeerAddress {
    std::string name;
    std::string host;
    std::uint16_t port = 0;
    bool advertised = false; // the peer can accept connections from the internet
    std::uint16_t voicePort = 0;
};

// Reads a Peer or PeerJoined frame.
bool parsePeerAddress(const Message& message, PeerAddress& out);
Message peerMessage(MsgType type, const PeerAddress& peer);

} // namespace chat
