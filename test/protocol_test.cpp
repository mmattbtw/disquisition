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

    CHECK(chat::parsePeerAddress(Message{MsgType::PeerJoined, {"bob", "h", "1", "0"}}, peer));
    CHECK(!peer.advertised);

    CHECK(!chat::parsePeerAddress(Message{MsgType::Peer, {"bob", "h", "0", "1"}}, peer));
    CHECK(!chat::parsePeerAddress(Message{MsgType::Peer, {"bob", "h", "65536", "1"}}, peer));
    CHECK(!chat::parsePeerAddress(Message{MsgType::Peer, {"bob", "h", "4000"}}, peer));
    CHECK(!chat::parsePeerAddress(Message{MsgType::PeerLeft, {"bob", "h", "4000", "1"}}, peer));

    const Message rebuilt = chat::peerMessage(MsgType::PeerJoined, {"bob", "h", 4000, true});
    CHECK(rebuilt.type == MsgType::PeerJoined);
    CHECK((rebuilt.fields == std::vector<std::string>{"bob", "h", "4000", "1"}));
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
    std::puts("protocol_test: ok");
    return 0;
}
