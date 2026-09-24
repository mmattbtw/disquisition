#include "common/protocol.h"

#include <algorithm>
#include <utility>

namespace chat {
namespace {

constexpr std::size_t kLengthSize = 4;
// Keeps value * 10 from overflowing int64 while parsing.
constexpr std::size_t kMaxDigits = 18;

void appendU32(std::string& out, std::uint32_t value) {
    out.push_back(static_cast<char>((value >> 24) & 0xFF));
    out.push_back(static_cast<char>((value >> 16) & 0xFF));
    out.push_back(static_cast<char>((value >> 8) & 0xFF));
    out.push_back(static_cast<char>(value & 0xFF));
}

std::uint32_t readU32(const char* data) {
    const auto byte = [data](int index) {
        return static_cast<std::uint32_t>(static_cast<unsigned char>(data[index]));
    };
    return (byte(0) << 24) | (byte(1) << 16) | (byte(2) << 8) | byte(3);
}

bool isControl(char character) {
    const auto value = static_cast<unsigned char>(character);
    return value < 0x20 || value == 0x7F;
}

} // namespace

std::string encode(const Message& message) {
    std::string payload;
    payload.push_back(static_cast<char>(message.type));
    for (const std::string& field : message.fields) {
        appendU32(payload, static_cast<std::uint32_t>(field.size()));
        payload.append(field);
    }

    std::string frame;
    frame.reserve(kLengthSize + payload.size());
    appendU32(frame, static_cast<std::uint32_t>(payload.size()));
    frame.append(payload);
    return frame;
}

DecodeStatus decode(std::string& buffer, Message& out) {
    if (buffer.size() < kLengthSize) {
        return DecodeStatus::Incomplete;
    }
    const std::uint32_t size = readU32(buffer.data());
    if (size == 0 || size > kMaxFrameSize) {
        return DecodeStatus::Malformed;
    }
    if (buffer.size() < kLengthSize + size) {
        return DecodeStatus::Incomplete;
    }

    const char* cursor = buffer.data() + kLengthSize;
    std::size_t remaining = size;

    Message message;
    message.type = static_cast<MsgType>(static_cast<unsigned char>(*cursor));
    ++cursor;
    --remaining;

    while (remaining > 0) {
        if (remaining < kLengthSize) {
            return DecodeStatus::Malformed;
        }
        const std::uint32_t length = readU32(cursor);
        cursor += kLengthSize;
        remaining -= kLengthSize;
        if (length > remaining) {
            return DecodeStatus::Malformed;
        }
        message.fields.emplace_back(cursor, length);
        cursor += length;
        remaining -= length;
    }

    buffer.erase(0, kLengthSize + size);
    out = std::move(message);
    return DecodeStatus::Ok;
}

bool parseColorIndex(const std::string& color, int& index) {
    if (color.empty() || color.size() > 3) {
        return false;
    }
    int value = 0;
    for (const char digit : color) {
        if (digit < '0' || digit > '9') {
            return false;
        }
        value = value * 10 + (digit - '0');
    }
    if (value > 255) {
        return false;
    }
    index = value;
    return true;
}

bool isReservedSystemColor(int index) {
    return index == 1 || index == 2 || index == 250;
}

bool isValidColor(const std::string& color) {
    if (std::find(kColorNames.begin(), kColorNames.end(), color) != kColorNames.end()) {
        return true;
    }
    int index = 0;
    return parseColorIndex(color, index) && !isReservedSystemColor(index);
}

std::string sanitizeName(const std::string& name) {
    std::string cleaned = trim(name);
    if (cleaned.size() > kMaxNameLength) {
        cleaned.resize(kMaxNameLength);
    }
    for (char& character : cleaned) {
        if (character == ' ' || character == '\t') {
            character = '_';
        }
        else if (isControl(character)) {
            character = '?';
        }
    }
    return trim(cleaned);
}

std::string sanitizeBody(const std::string& body) {
    std::string cleaned = body;
    std::replace_if(cleaned.begin(), cleaned.end(), isControl, ' ');
    cleaned = trim(cleaned);
    if (cleaned.size() > kMaxBodyLength) {
        cleaned.resize(kMaxBodyLength);
    }
    return cleaned;
}

std::string trim(const std::string& text) {
    constexpr const char* kWhitespace = " \t\r\n";
    const auto begin = text.find_first_not_of(kWhitespace);
    if (begin == std::string::npos) {
        return "";
    }
    const auto end = text.find_last_not_of(kWhitespace);
    return text.substr(begin, end - begin + 1);
}

bool parseInt64(const std::string& text, std::int64_t& out) {
    std::size_t index = 0;
    const bool negative = !text.empty() && text[0] == '-';
    if (!text.empty() && (text[0] == '-' || text[0] == '+')) {
        index = 1;
    }
    const std::size_t digits = text.size() - index;
    if (digits == 0 || digits > kMaxDigits) {
        return false;
    }

    std::int64_t value = 0;
    for (; index < text.size(); ++index) {
        if (text[index] < '0' || text[index] > '9') {
            return false;
        }
        value = value * 10 + (text[index] - '0');
    }
    out = negative ? -value : value;
    return true;
}

bool parsePort(const std::string& text, std::uint16_t& out, bool allowZero) {
    std::int64_t value = 0;
    const std::int64_t minimum = allowZero ? 0 : 1;
    if (!parseInt64(text, value) || value < minimum || value > 65535) {
        return false;
    }
    out = static_cast<std::uint16_t>(value);
    return true;
}

bool parsePeerChat(const Message& message, ChatPayload& out) {
    if (message.type != MsgType::PeerChat || message.fields.size() < 4) {
        return false;
    }
    std::int64_t timestamp = 0;
    std::string body = sanitizeBody(message.fields[2]);
    std::string color = sanitizeBody(message.fields[3]);
    if (!parseInt64(message.fields[1], timestamp) || timestamp <= 0 || body.empty() ||
        !isValidColor(color)) {
        return false;
    }
    out.sender = message.fields[0];
    out.timestamp = timestamp;
    out.body = std::move(body);
    out.color = std::move(color);
    return true;
}

bool parsePeerAddress(const Message& message, PeerAddress& out) {
    if ((message.type != MsgType::Peer && message.type != MsgType::PeerJoined) ||
        message.fields.size() < 4) {
        return false;
    }
    std::uint16_t port = 0;
    if (!parsePort(message.fields[2], port, false)) {
        return false;
    }
    out.name = message.fields[0];
    out.host = message.fields[1];
    out.port = port;
    out.advertised = message.fields[3] == "1";
    return true;
}

Message peerMessage(MsgType type, const PeerAddress& peer) {
    return Message{type,
                   {peer.name, peer.host, std::to_string(peer.port), peer.advertised ? "1" : "0"}};
}

} // namespace chat
