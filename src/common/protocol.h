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
    Store = 10,        // [timestamp, body] archive a message already sent p2p
    FetchHistory = 11, // []

    // server -> client
    LoginOk = 3,    // [assigned name, welcome text]
    Error = 4,      // [reason]
    History = 6,    // [timestamp, sender, body]
    System = 7,     // [text]
    Users = 8,      // [name]*
    HistoryEnd = 9, // []
    Peer = 12,      // [name, host, port, advertised (0|1)] one roster entry
    PeerJoined = 13, // [name, host, port, advertised (0|1)]
    PeerLeft = 14,  // [name]

    // peer -> peer
    Hello = 15,    // [name] sent once by whichever side dialled
    PeerChat = 16  // [timestamp, body] the sender owns the connection
};

// Hard cap on a single frame so a hostile client cannot make us allocate.
constexpr std::uint32_t kMaxFrameSize = 16 * 1024;

// Limits shared by the server and every peer so both ends agree on them.
constexpr std::size_t kMaxNameLength = 20;
constexpr std::size_t kMaxBodyLength = 2000;

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

}  // namespace chat
