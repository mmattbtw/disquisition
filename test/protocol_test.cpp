#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "check.h"
#include "common/protocol.h"

namespace {

using chat::DecodeStatus;
using chat::Message;
using chat::MsgType;

std::string bigEndian(std::uint32_t value) {
    std::string bytes;
    bytes.push_back(static_cast<char>((value >> 24) & 0xFF));
    bytes.push_back(static_cast<char>((value >> 16) & 0xFF));
    bytes.push_back(static_cast<char>((value >> 8) & 0xFF));
    bytes.push_back(static_cast<char>(value & 0xFF));
    return bytes;
}

// Builds a frame by hand so the tests do not depend on encode being right.
std::string rawFrame(const std::string& payload) {
    return bigEndian(static_cast<std::uint32_t>(payload.size())) + payload;
}

void testEncodeLayout() {
    const std::string frame = chat::encode(Message{MsgType::Login, {"matt", "1", ""}});
    const std::string expected = rawFrame(std::string("\x01", 1) + std::string("\0\0\0\x04", 4) +
                                          "matt" + std::string("\0\0\0\x01", 4) + "1" +
                                          std::string("\0\0\0\0", 4));
    CHECK(frame == expected);
}

void testRoundTrip() {
    const Message original{MsgType::PeerChat, {"jesse", "123", std::string("a\0b", 3), ""}};
    std::string buffer = chat::encode(original);
    Message decoded;
    CHECK(chat::decode(buffer, decoded) == DecodeStatus::Ok);
    CHECK(buffer.empty());
    CHECK(decoded.type == original.type);
    CHECK(decoded.fields == original.fields);
}

void testEmptyMessage() {
    std::string buffer = chat::encode(Message{MsgType::FetchHistory, {}});
    CHECK(buffer.size() == 5);
    Message decoded;
    CHECK(chat::decode(buffer, decoded) == DecodeStatus::Ok);
    CHECK(decoded.type == MsgType::FetchHistory);
    CHECK(decoded.fields.empty());
}

void testPartialAndBackToBackFrames() {
    const std::string first = chat::encode(Message{MsgType::System, {"one"}});
    const std::string second = chat::encode(Message{MsgType::System, {"two"}});
    const std::string stream = first + second;

    // Feed one byte at a time: every prefix of a frame is Incomplete and the
    // buffer is left untouched until the frame is whole.
    std::string buffer;
    std::vector<std::string> seen;
    for (const char byte : stream) {
        buffer.push_back(byte);
        Message decoded;
        const std::size_t before = buffer.size();
        const DecodeStatus status = chat::decode(buffer, decoded);
        if (status == DecodeStatus::Incomplete) {
            CHECK(buffer.size() == before);
            continue;
        }
        CHECK(status == DecodeStatus::Ok);
        seen.push_back(decoded.fields.at(0));
    }
    CHECK((seen == std::vector<std::string>{"one", "two"}));
    CHECK(buffer.empty());

    // Two frames arriving in one read decode one at a time.
    buffer = stream;
    Message decoded;
    CHECK(chat::decode(buffer, decoded) == DecodeStatus::Ok);
    CHECK(decoded.fields.at(0) == "one");
    CHECK(buffer == second);
    CHECK(chat::decode(buffer, decoded) == DecodeStatus::Ok);
    CHECK(decoded.fields.at(0) == "two");
}

void testMalformedFrames() {
    Message decoded;

    std::string empty = rawFrame("");
    CHECK(chat::decode(empty, decoded) == DecodeStatus::Malformed);

    std::string huge = rawFrame(std::string(chat::kMaxFrameSize + 1, 'x'));
    CHECK(chat::decode(huge, decoded) == DecodeStatus::Malformed);

    // A field length that runs past the end of the payload.
    std::string overrun = rawFrame(std::string("\x07\0\0\0\x09", 5) + "abc");
    CHECK(chat::decode(overrun, decoded) == DecodeStatus::Malformed);

    // Trailing bytes too short to hold a field length.
    std::string truncated = rawFrame(std::string("\x07\0\0", 3));
    CHECK(chat::decode(truncated, decoded) == DecodeStatus::Malformed);

    // The largest legal frame still decodes: one type byte, one length and
    // a field that fills the rest of the payload.
    const std::uint32_t fieldLength = chat::kMaxFrameSize - 5;
    std::string largest = rawFrame(std::string("\x07", 1) + bigEndian(fieldLength) +
                                   std::string(fieldLength, 'x'));
    CHECK(chat::decode(largest, decoded) == DecodeStatus::Ok);
    CHECK(decoded.fields.at(0).size() == fieldLength);
}

void testSanitize() {
    CHECK(chat::sanitizeName("  matt  ") == "matt");
    CHECK(chat::sanitizeName("big matt") == "big_matt");
    CHECK(chat::sanitizeName("a\tb") == "a_b");
    CHECK(chat::sanitizeName("bell\x07") == "bell?");
    CHECK(chat::sanitizeName("   ") == "");
    CHECK(chat::sanitizeName(std::string(50, 'n')).size() == chat::kMaxNameLength);

    CHECK(chat::sanitizeBody("  hi\tthere\n") == "hi there");
    CHECK(chat::sanitizeBody("a\x1b[31mb") == "a [31mb");
    CHECK(chat::sanitizeBody(std::string(5000, 'b')).size() == chat::kMaxBodyLength);
    CHECK(chat::sanitizeBody("\r\n") == "");
}

void testTrim() {
    CHECK(chat::trim("") == "");
    CHECK(chat::trim(" \t\r\n") == "");
    CHECK(chat::trim("  a b  ") == "a b");
    CHECK(chat::trim("x") == "x");
}

void testParseNumbers() {
    std::int64_t value = 0;
    CHECK(chat::parseInt64("0", value) && value == 0);
    CHECK(chat::parseInt64("-42", value) && value == -42);
    CHECK(chat::parseInt64("+7", value) && value == 7);
    CHECK(chat::parseInt64("999999999999999999", value) && value == 999999999999999999LL);
    CHECK(!chat::parseInt64("1000000000000000000", value));
    CHECK(!chat::parseInt64("", value));
    CHECK(!chat::parseInt64("-", value));
    CHECK(!chat::parseInt64("12a", value));
    CHECK(!chat::parseInt64(" 1", value));

    std::uint16_t port = 0;
    CHECK(chat::parsePort("9000", port, false) && port == 9000);
    CHECK(chat::parsePort("65535", port, false) && port == 65535);
    CHECK(!chat::parsePort("65536", port, false));
    CHECK(!chat::parsePort("0", port, false));
    CHECK(chat::parsePort("0", port, true) && port == 0);
    CHECK(!chat::parsePort("-1", port, true));
}

void testColors() {
    for (const char* name : chat::kColorNames) {
        CHECK(chat::isValidColor(name));
    }
    CHECK(chat::isValidColor("0"));
    CHECK(chat::isValidColor("20"));
    CHECK(chat::isValidColor("255"));
    CHECK(!chat::isValidColor("1"));
    CHECK(!chat::isValidColor("2"));
    CHECK(!chat::isValidColor("250"));
    CHECK(!chat::isValidColor("256"));
    CHECK(!chat::isValidColor("0020"));
    CHECK(!chat::isValidColor("red"));
    CHECK(!chat::isValidColor(""));
}

void testPeerChat() {
    chat::ChatPayload chat;
    CHECK(chat::parsePeerChat(Message{MsgType::PeerChat, {"jesse", "5", " hi\t", "pink"}}, chat));
    CHECK(chat.sender == "jesse");
    CHECK(chat.timestamp == 5);
    CHECK(chat.body == "hi");
    CHECK(chat.color == "pink");

    CHECK(!chat::parsePeerChat(Message{MsgType::PeerChat, {"jesse", "5", "hi"}}, chat));
    CHECK(!chat::parsePeerChat(Message{MsgType::PeerChat, {"jesse", "0", "hi", "pink"}}, chat));
    CHECK(!chat::parsePeerChat(Message{MsgType::PeerChat, {"jesse", "x", "hi", "pink"}}, chat));
    CHECK(!chat::parsePeerChat(Message{MsgType::PeerChat, {"jesse", "5", "  ", "pink"}}, chat));
    CHECK(!chat::parsePeerChat(Message{MsgType::PeerChat, {"jesse", "5", "hi", "250"}}, chat));
    CHECK(!chat::parsePeerChat(Message{MsgType::Store, {"jesse", "5", "hi", "pink"}}, chat));
}

void testPeerAnnouncement() {
    chat::PeerAddress peer;
    CHECK(chat::parsePeerAddress(Message{MsgType::Peer, {"bob", "10.0.0.2", "4000", "1"}}, peer));
    CHECK(peer.name == "bob");
    CHECK(peer.host == "10.0.0.2");
    CHECK(peer.port == 4000);
    CHECK(peer.advertised);
    CHECK(peer.voicePort == 0);

    CHECK(chat::parsePeerAddress(
        Message{MsgType::Peer, {"bob", "10.0.0.2", "4000", "1", "5060"}}, peer));
    CHECK(peer.voicePort == 5060);
    CHECK(!chat::parsePeerAddress(
        Message{MsgType::Peer, {"bob", "10.0.0.2", "4000", "1", "65536"}}, peer));

    CHECK(chat::parsePeerAddress(Message{MsgType::PeerJoined, {"bob", "h", "1", "0"}}, peer));
    CHECK(!peer.advertised);

    CHECK(!chat::parsePeerAddress(Message{MsgType::Peer, {"bob", "h", "0", "1"}}, peer));
    CHECK(!chat::parsePeerAddress(Message{MsgType::Peer, {"bob", "h", "65536", "1"}}, peer));
    CHECK(!chat::parsePeerAddress(Message{MsgType::Peer, {"bob", "h", "4000"}}, peer));
    CHECK(!chat::parsePeerAddress(Message{MsgType::PeerLeft, {"bob", "h", "4000", "1"}}, peer));

    const Message rebuilt = chat::peerMessage(MsgType::PeerJoined, {"bob", "h", 4000, true});
    CHECK(rebuilt.type == MsgType::PeerJoined);
    CHECK((rebuilt.fields == std::vector<std::string>{"bob", "h", "4000", "1"}));
    const Message voiced = chat::peerMessage(MsgType::Peer, {"bob", "h", 4000, true, 5060});
    CHECK((voiced.fields == std::vector<std::string>{"bob", "h", "4000", "1", "5060"}));
}

void testPeerImage() {
    using Fields = std::vector<std::string>;
    const std::string bytes(chat::kImageChunkSize, 'x');
    chat::ImageChunk chunk;
    CHECK(chat::parsePeerImage(
        Message{MsgType::PeerImage, {"bob", "123", "pink", "4294967295", "2", "3", bytes}}, chunk));
    CHECK(chunk.sender == "bob");
    CHECK(chunk.timestamp == 123);
    CHECK(chunk.color == "pink");
    CHECK(chunk.transferId == 4294967295u);
    CHECK(chunk.index == 2);
    CHECK(chunk.count == 3);
    CHECK(chunk.data == bytes);

    // The largest chunk still fits in one frame.
    std::string buffer = chat::encode(chat::peerImageMessage(chunk));
    Message decoded;
    CHECK(chat::decode(buffer, decoded) == DecodeStatus::Ok);
    CHECK((decoded.fields == Fields{"bob", "123", "pink", "4294967295", "2", "3", bytes}));

    const auto rejects = [&](Fields fields) {
        return !chat::parsePeerImage(Message{MsgType::PeerImage, std::move(fields)}, chunk);
    };
    const std::string count = std::to_string(chat::kMaxImageChunks);
    CHECK(chat::parsePeerImage(
        Message{MsgType::PeerImage, {"bob", "1", "pink", "1", "0", count, "x"}}, chunk));
    CHECK(rejects({"bob", "1", "pink", "1", "0", std::to_string(chat::kMaxImageChunks + 1), "x"}));
    CHECK(rejects({"bob", "1", "pink", "1", "3", "3", "x"}));
    CHECK(rejects({"bob", "1", "pink", "1", "0", "0", "x"}));
    CHECK(rejects({"bob", "1", "pink", "1", "0", "1", ""}));
    CHECK(rejects({"bob", "1", "pink", "1", "0", "1", bytes + "x"}));
    CHECK(rejects({"bob", "0", "pink", "1", "0", "1", "x"}));
    CHECK(rejects({"bob", "1", "1", "1", "0", "1", "x"}));
    CHECK(rejects({"bob", "1", "pink", "-1", "0", "1", "x"}));
    CHECK(rejects({"bob", "1", "pink", "4294967296", "0", "1", "x"}));
    CHECK(rejects({"bob", "1", "pink", "1", "+0", "1", "x"}));
    CHECK(rejects({"bob", "1", "pink", "1", "0", "1"}));
    CHECK(!chat::parsePeerImage(
        Message{MsgType::PeerChat, {"bob", "1", "pink", "1", "0", "1", "x"}}, chunk));
}

void testScreenDraw() {
    chat::ScreenDraw draw;
    const Message valid{MsgType::ScreenDraw, {"alice", "7", "12", "0", "10000", "123", "456", "5"}};
    CHECK(chat::parseScreenDraw(valid, false, draw));
    CHECK(draw.sharer == "alice" && draw.epoch == 7 && draw.id == 12);
    CHECK(draw.x1 == 0 && draw.y1 == 10000 && draw.x2 == 123 && draw.y2 == 456 && draw.color == 5);
    CHECK(chat::screenDrawMessage(draw, false).fields == valid.fields);
    draw.author = "bob";
    const Message forwarded = chat::screenDrawMessage(draw, true);
    CHECK(forwarded.fields[2] == "bob");
    CHECK(chat::parseScreenDraw(forwarded, true, draw));
    CHECK(draw.author == "bob");
    CHECK(!chat::parseScreenDraw(forwarded, false, draw));
    CHECK(!chat::parseScreenDraw(valid, true, draw));
    for (std::size_t field = 1; field < valid.fields.size(); ++field) {
        for (const std::string bad : {"", "-1", "+1", "1.5", "nan", "99999999999999999999"}) {
            Message invalid = valid;
            invalid.fields[field] = bad;
            // Epochs use the existing signed decimal parser, which accepts '+'.
            if (field == 1 && bad == "+1")
                continue;
            CHECK(!chat::parseScreenDraw(invalid, false, draw));
        }
    }
    for (std::size_t field = 3; field <= 7; ++field) {
        Message invalid = valid;
        invalid.fields[field] = field == 7 ? "6" : "10001";
        CHECK(!chat::parseScreenDraw(invalid, false, draw));
    }
    Message invalid = valid;
    invalid.fields[1] = "0";
    CHECK(!chat::parseScreenDraw(invalid, false, draw));
    invalid = valid;
    invalid.fields[2] = "4294967296";
    CHECK(!chat::parseScreenDraw(invalid, false, draw));
    invalid = valid;
    invalid.type = MsgType::PeerChat;
    CHECK(!chat::parseScreenDraw(invalid, false, draw));
}

} // namespace

int main() {
    testEncodeLayout();
    testRoundTrip();
    testEmptyMessage();
    testPartialAndBackToBackFrames();
    testMalformedFrames();
    testSanitize();
    testTrim();
    testParseNumbers();
    testColors();
    testPeerChat();
    testPeerAnnouncement();
    testPeerImage();
    testScreenDraw();
    std::puts("protocol_test: ok");
    return 0;
}
