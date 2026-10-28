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
// All integers are big-endian. The server handles sign-in and discovery;
// live chat travels directly between peers (or through a relay) and
// never passes through the server.
namespace chat {

enum class MsgType : std::uint8_t {
    // Client -> server
    Login = 1,         // [name, peer port, advertised host (may be empty)]
    Store = 10,        // [timestamp, body, color]  legacy storage request, discarded
    FetchHistory = 11, // [] legacy request, returns only HistoryEnd
    VoicePort = 19,    // client: [SIP port, 0 to leave]; server: [name, SIP port]
    VoiceAudio = 20,   // client: [16 kHz mono signed-16 PCM]; server: [sender, PCM]

    // Server -> client
    LoginOk = 3,     // [assigned name, welcome text, features (may be absent)]
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
    Hello = 15,      // [sender, target]
    HelloOk = 17,    // [target]
    PeerChat = 16,   // [sender, timestamp, body, color]
    VoiceState = 18, // [sender, muted 0|1, deafened 0|1]
    RelayProbe = 21, // client to relay: [] health check without signing in
    RelayReady = 22, // relay to client: [] server connection is available

    // Screen sharing. Video travels through the server, which forwards each
    // sharer's frames only to the users watching them.
    ScreenShare = 23, // client: [1 sharing | 0 stopped]; server: [sharer, 1|0]
    ScreenWatch = 24, // client to server: [sharer, 1 watch | 0 stop watching]
    // client: [frame id, chunk index, chunk count, keyframe 0|1, H.264 bytes];
    // server: [sharer, frame id, chunk index, chunk count, keyframe 0|1, H.264 bytes]
    ScreenFrame = 25,

    // Peer <-> peer. An image too large for one frame travels as numbered
    // chunks of the same transfer, each carrying the whole header:
    // [sender, timestamp, color, transfer id, chunk index, chunk count, bytes]
    PeerImage = 26,

    // Drawing messages are client-owned and travel inside PeerData, never to
    // the discovery server. State resets the drawing before any history replay.
    // Owner -> viewer: [sharer, enabled 0|1, epoch, optional may-draw 0|1].
    ScreenDrawState = 27,
    // Viewer -> owner: [sharer, epoch, segment id, x1, y1, x2, y2, color];
    // owner -> viewers inserts author at index 2. Author + id deduplicates previews.
    // Coordinates are normalized to 0..10000, color is an index in the pen palette.
    // New clients append [stroke id, RGB or -1] to group gestures and carry chat colors.
    ScreenDraw = 28,
    ScreenDrawWatch = 29, // viewer -> owner: [sharer, 1 subscribe | 0 unsubscribe]
    // Generic targeted peer channel: [sender, target, channel, binary payload].
    // The relay stamps sender using the established peer/client identity.
    PeerData = 30,
    PeerLink = 31, // relay -> client: [peer, connected 0|1], for resuming peer channels
    ScreenDrawErase = 32, // peer data: [sharer, epoch, author, stroke ids...]
    // Transient peer data: [sharer, epoch, optional owner-stamped author,
    // sequence, x, y, RGB, visible 0|1]. Never included in drawing history.
    ScreenLaser = 33,
    Speaking = 34, // client: [1 talking | 0 quiet]; server: [name, 1|0]

    // Personal desktop sprite, via the server. Client: [index, count, bytes];
    // server: [assigned sender, index, count, bytes]. Empty fields reset to
    // Club Penguin; the server's reset contains only the assigned sender.
    PenguinSprite = 35,

    // The sound of a screen share, forwarded like ScreenFrame to its viewers.
    // Each packet is 20 ms of 48 kHz stereo Opus. Servers that take it list
    // kScreenAudioFeature. Client: [Opus packet]; server: [sharer, Opus packet].
    ScreenAudio = 36,
};

struct Message {
    MsgType type = MsgType::Error;
    std::vector<std::string> fields;
};

constexpr std::uint32_t kMaxFrameSize = 16 * 1024;
constexpr std::size_t kMaxPeerDataBytes = kMaxFrameSize - 256;
constexpr std::size_t kMaxNameLength = 20;
constexpr std::size_t kMaxBodyLength = 2000;
// Image chunks leave room for the header fields inside kMaxFrameSize.
constexpr std::size_t kImageChunkSize = 12 * 1024;
constexpr std::size_t kMaxImageBytes = 4 * 1024 * 1024;
constexpr std::uint32_t kMaxImageChunks =
    static_cast<std::uint32_t>((kMaxImageBytes + kImageChunkSize - 1) / kImageChunkSize);
constexpr std::size_t kMaxSpriteBytes = 256 * 1024;
constexpr std::uint32_t kMaxSpriteChunks =
    static_cast<std::uint32_t>((kMaxSpriteBytes + kImageChunkSize - 1) / kImageChunkSize);
// A 20 ms packet at 128 kbps is about 320 bytes; this leaves headroom.
constexpr std::size_t kMaxScreenAudioBytes = 1500;

// TCP delivers chunks in order. Only a complete, bounded transfer replaces
// the previous sprite. A new index 0 starts a replacement; a reset is complete.
class SpriteTransfer {
public:
    bool accept(const Message& message, bool fromServer = false);
    bool complete() const { return next_ == count_; }
    const std::string& data() const { return data_; }

private:
    std::string data_;
    std::uint32_t next_ = 0;
    std::uint32_t count_ = 0;
};

std::vector<Message> spriteMessages(const std::string& data, const std::string& sender = {});

// Messages carry their color as a string: one of these names, or an
// xterm-256 index such as "208".
constexpr std::array<const char*, 7> kColorNames = {"pink",  "mint", "butter", "periwinkle",
                                                    "lilac", "aqua", "peach"};
constexpr std::size_t kColorCount = kColorNames.size();

// LoginOk's third field lists, space separated, the messages a server takes
// beyond the original protocol. Older servers disconnect anyone who sends a
// message they do not know, so clients check this list first.
constexpr const char* kSpeakingFeature = "speaking";
constexpr const char* kScreenAudioFeature = "screen-audio";
bool hasFeature(const std::string& features, const std::string& feature);

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
// Color indexes reserved for system notices by the wire protocol.
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

struct ImageChunk {
    std::string sender; // as written by the sender; not verified
    std::int64_t timestamp = 0;
    std::string color;
    std::uint32_t transferId = 0;
    std::uint32_t index = 0;
    std::uint32_t count = 0;
    std::string data;
};

// Validates a PeerImage frame: positive timestamp, valid color, index within
// count, count within kMaxImageChunks, and 1 to kImageChunkSize bytes.
bool parsePeerImage(const Message& message, ImageChunk& out);
Message peerImageMessage(const ImageChunk& chunk);

struct PeerData {
    std::string sender;
    std::string target;
    std::string channel;
    std::string data;
};
bool parsePeerData(const Message& message, PeerData& out);
Message peerDataMessage(const PeerData& data);

constexpr int kDrawCoordinateMax = 10000;
constexpr int kPenColorCount = 6;
constexpr std::size_t kMaxDrawSegments = 4096;
struct ScreenDraw {
    std::string sharer;
    std::string author;
    std::int64_t epoch = 0;
    std::uint32_t id = 0;
    int x1 = 0, y1 = 0, x2 = 0, y2 = 0;
    int color = 0;
    std::uint32_t stroke = 0; // one continuous gesture; 0 uses the legacy segment id
    int rgb = -1; // 24-bit RGB; -1 uses the legacy palette
};
bool parseScreenDraw(const Message& message, bool fromOwner, ScreenDraw& out);
Message screenDrawMessage(const ScreenDraw& draw, bool fromOwner);

constexpr std::size_t kMaxEraseStrokes = 128;
struct ScreenErase {
    std::string sharer;
    std::string author;
    std::int64_t epoch = 0;
    std::vector<std::uint32_t> strokes;
};
bool parseScreenErase(const Message& message, ScreenErase& out);
Message screenEraseMessage(const ScreenErase& erase);

struct ScreenLaser {
    std::string sharer;
    std::string author;
    std::int64_t epoch = 0;
    std::uint32_t id = 0;
    int x = 0, y = 0;
    int rgb = 0;
    bool visible = true;
};
bool parseScreenLaser(const Message& message, bool fromOwner, ScreenLaser& out);
Message screenLaserMessage(const ScreenLaser& laser, bool fromOwner);

struct PeerAddress {
    std::string name;
    std::string host;
    std::uint16_t port = 0;
    bool advertised = false; // the peer supplied an address for direct connections
    std::uint16_t voicePort = 0;
};

// Reads a Peer or PeerJoined frame.
bool parsePeerAddress(const Message& message, PeerAddress& out);
Message peerMessage(MsgType type, const PeerAddress& peer);

} // namespace chat
