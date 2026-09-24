#include "common/protocol.h"

namespace chat {
namespace {

// Add a 32-bit number to a byte string in big-endian order. Each shift moves
// the wanted byte into the lowest position. The mask keeps only that byte.
void appendU32(std::string& out, std::uint32_t value) {
    out.push_back(static_cast<char>((value >> 24) & 0xFF));
    out.push_back(static_cast<char>((value >> 16) & 0xFF));
    out.push_back(static_cast<char>((value >> 8) & 0xFF));
    out.push_back(static_cast<char>(value & 0xFF));
}

// Read the four bytes written by appendU32 and rebuild the original number.
// unsigned char prevents a byte above 127 from being treated as negative.
std::uint32_t readU32(const char* data) {
    return (static_cast<std::uint32_t>(static_cast<unsigned char>(data[0])) << 24) |
           (static_cast<std::uint32_t>(static_cast<unsigned char>(data[1])) << 16) |
           (static_cast<std::uint32_t>(static_cast<unsigned char>(data[2])) << 8) |
           static_cast<std::uint32_t>(static_cast<unsigned char>(data[3]));
}

// Remove spaces, tabs, and line endings from both ends of a string. Whitespace
// inside the string stays where it is.
std::string trimWhitespace(const std::string& text) {
    const auto begin = text.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos) {
        // The string was empty or contained only whitespace.
        return "";
    }
    const auto end = text.find_last_not_of(" \t\r\n");
    return text.substr(begin, end - begin + 1);
}

}  // namespace

std::string encode(const Message& message) {
    // Build the payload first. Its first byte is the message type. Every field
    // starts with its byte count so decode knows exactly where that field ends.
    std::string payload;
    payload.push_back(static_cast<char>(message.type));
    for (const std::string& field : message.fields) {
        appendU32(payload, static_cast<std::uint32_t>(field.size()));
        payload.append(field);
    }

    // The completed frame starts with the payload size. reserve avoids extra
    // memory allocations while the strings are joined.
    std::string frame;
    frame.reserve(payload.size() + 4);
    appendU32(frame, static_cast<std::uint32_t>(payload.size()));
    frame.append(payload);
    return frame;
}

DecodeStatus decode(std::string& buffer, Message& out) {
    // The first four bytes hold the payload size. Without all four, there is
    // not enough information to decide how long the frame should be.
    if (buffer.size() < 4) {
        return DecodeStatus::Incomplete;
    }

    const std::uint32_t size = readU32(buffer.data());
    // A payload must contain at least its one-byte message type. The upper
    // limit protects memory use and matches the check documented in the header.
    if (size == 0 || size > kMaxFrameSize) {
        return DecodeStatus::Malformed;
    }
    if (buffer.size() < 4 + size) {
        // The size is valid, but TCP has not delivered the whole frame yet.
        return DecodeStatus::Incomplete;
    }

    // cursor points at the next unread byte. remaining records how many bytes
    // are left in this payload. The first payload byte is always the type.
    const char* cursor = buffer.data() + 4;
    std::size_t remaining = size;

    Message message;
    message.type = static_cast<MsgType>(static_cast<unsigned char>(cursor[0]));
    ++cursor;
    --remaining;

    while (remaining > 0) {
        // Every field needs a four-byte length before its text.
        if (remaining < 4) {
            return DecodeStatus::Malformed;
        }
        const std::uint32_t length = readU32(cursor);
        cursor += 4;
        remaining -= 4;
        // A field cannot be longer than the unread part of the payload.
        if (length > remaining) {
            return DecodeStatus::Malformed;
        }
        message.fields.emplace_back(cursor, length);
        cursor += length;
        remaining -= length;
    }

    // Remove the decoded frame. Any later frame stays in buffer.
    buffer.erase(0, 4 + size);
    out = std::move(message);
    return DecodeStatus::Ok;
}

const char* typeName(MsgType type) {
    // Returning string literals is safe because they exist for the entire run
    // of the program. The caller does not need to free them.
    switch (type) {
        case MsgType::Login: return "Login";
        case MsgType::Store: return "Store";
        case MsgType::FetchHistory: return "FetchHistory";
        case MsgType::LoginOk: return "LoginOk";
        case MsgType::Error: return "Error";
        case MsgType::History: return "History";
        case MsgType::System: return "System";
        case MsgType::Users: return "Users";
        case MsgType::HistoryEnd: return "HistoryEnd";
        case MsgType::Peer: return "Peer";
        case MsgType::PeerJoined: return "PeerJoined";
        case MsgType::PeerLeft: return "PeerLeft";
        case MsgType::Hello: return "Hello";
        case MsgType::HelloOk: return "HelloOk";
        case MsgType::PeerChat: return "PeerChat";
        case MsgType::VoiceState: return "VoiceState";
        case MsgType::VoicePort: return "VoicePort";
        case MsgType::VoiceAudio: return "VoiceAudio";
        case MsgType::RelayProbe: return "RelayProbe";
        case MsgType::RelayReady: return "RelayReady";
    }
    return "Unknown";
}

std::string sanitizeName(const std::string& name) {
    // Start by removing whitespace around the name, then apply the shared
    // length limit before replacing characters that should not appear in it.
    std::string cleaned = trimWhitespace(name);
    if (cleaned.size() > kMaxNameLength) {
        cleaned.resize(kMaxNameLength);
    }
    for (char& character : cleaned) {
        if (character == ' ' || character == '\t') {
            character = '_';
        } else if (static_cast<unsigned char>(character) < 0x20 || character == 0x7F) {
            character = '?';
        }
    }
    return trimWhitespace(cleaned);
}

std::string sanitizeBody(const std::string& body) {
    // Messages may contain normal spaces, but terminal control characters
    // could damage the display. Replace those characters with plain spaces.
    std::string cleaned = body;
    for (char& character : cleaned) {
        const unsigned char value = static_cast<unsigned char>(character);
        if (character == '\t') {
            character = ' ';
        } else if (value < 0x20 || value == 0x7F) {
            character = ' ';
        }
    }
    cleaned = trimWhitespace(cleaned);
    if (cleaned.size() > kMaxBodyLength) {
        cleaned.resize(kMaxBodyLength);
    }
    return cleaned;
}

bool parseInt64(const std::string& text, std::int64_t& out) {
    // Reject input that is empty or too long before doing any arithmetic.
    if (text.empty() || text.size() > 19) {
        return false;
    }
    std::size_t index = 0;
    bool negative = false;
    if (text[0] == '-' || text[0] == '+') {
        // A sign is allowed only when at least one digit follows it.
        negative = text[0] == '-';
        index = 1;
        if (index >= text.size()) {
            return false;
        }
    }
    std::int64_t value = 0;
    std::size_t digits = 0;
    for (; index < text.size(); ++index, ++digits) {
        // Limiting the digit count keeps value * 10 inside int64_t. This parser
        // intentionally accepts fewer digits than the type's absolute maximum.
        if (digits >= 18 || text[index] < '0' || text[index] > '9') {
            return false;
        }
        value = value * 10 + (text[index] - '0');
    }
    out = negative ? -value : value;
    return true;
}

bool parsePort(const std::string& text, std::uint16_t& out, bool allowZero) {
    // Normal connections use ports 1 through 65535. A listening socket may
    // use port 0 to ask the operating system to choose an available port.
    std::int64_t value = 0;
    const std::int64_t minimum = allowZero ? 0 : 1;
    if (!parseInt64(text, value) || value < minimum || value > 65535) {
        return false;
    }
    out = static_cast<std::uint16_t>(value);
    return true;
}

}  // namespace chat
