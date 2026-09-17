#include "common/protocol.h"

namespace chat {
namespace {

void appendU32(std::string& out, std::uint32_t value) {
    out.push_back(static_cast<char>((value >> 24) & 0xFF));
    out.push_back(static_cast<char>((value >> 16) & 0xFF));
    out.push_back(static_cast<char>((value >> 8) & 0xFF));
    out.push_back(static_cast<char>(value & 0xFF));
}

std::uint32_t readU32(const char* data) {
    return (static_cast<std::uint32_t>(static_cast<unsigned char>(data[0])) << 24) |
           (static_cast<std::uint32_t>(static_cast<unsigned char>(data[1])) << 16) |
           (static_cast<std::uint32_t>(static_cast<unsigned char>(data[2])) << 8) |
           static_cast<std::uint32_t>(static_cast<unsigned char>(data[3]));
}

std::string trimWhitespace(const std::string& text) {
    const auto begin = text.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos) {
        return "";
    }
    const auto end = text.find_last_not_of(" \t\r\n");
    return text.substr(begin, end - begin + 1);
}

}  // namespace

std::string encode(const Message& message) {
    std::string payload;
    payload.push_back(static_cast<char>(message.type));
    for (const std::string& field : message.fields) {
        appendU32(payload, static_cast<std::uint32_t>(field.size()));
        payload.append(field);
    }

    std::string frame;
    frame.reserve(payload.size() + 4);
    appendU32(frame, static_cast<std::uint32_t>(payload.size()));
    frame.append(payload);
    return frame;
}

DecodeStatus decode(std::string& buffer, Message& out) {
    if (buffer.size() < 4) {
        return DecodeStatus::Incomplete;
    }

    const std::uint32_t size = readU32(buffer.data());
    if (size == 0 || size > kMaxFrameSize) {
        return DecodeStatus::Malformed;
    }
    if (buffer.size() < 4 + size) {
        return DecodeStatus::Incomplete;
    }

    const char* cursor = buffer.data() + 4;
    std::size_t remaining = size;

    Message message;
    message.type = static_cast<MsgType>(static_cast<unsigned char>(cursor[0]));
    ++cursor;
    --remaining;

    while (remaining > 0) {
        if (remaining < 4) {
            return DecodeStatus::Malformed;
        }
        const std::uint32_t length = readU32(cursor);
        cursor += 4;
        remaining -= 4;
        if (length > remaining) {
            return DecodeStatus::Malformed;
        }
        message.fields.emplace_back(cursor, length);
        cursor += length;
        remaining -= length;
    }

    buffer.erase(0, 4 + size);
    out = std::move(message);
    return DecodeStatus::Ok;
}

const char* typeName(MsgType type) {
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
    }
    return "Unknown";
}

std::string sanitizeName(const std::string& name) {
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
    if (text.empty() || text.size() > 19) {
        return false;
    }
    std::size_t index = 0;
    bool negative = false;
    if (text[0] == '-' || text[0] == '+') {
        negative = text[0] == '-';
        index = 1;
        if (index >= text.size()) {
            return false;
        }
    }
    std::int64_t value = 0;
    std::size_t digits = 0;
    for (; index < text.size(); ++index, ++digits) {
        if (digits >= 18 || text[index] < '0' || text[index] > '9') {
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

}  // namespace chat
