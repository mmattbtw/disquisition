#include "common/drawing_protocol.h"

#include <algorithm>
#include <utility>

namespace chat {
namespace {
constexpr std::size_t kLengthSize = 4;
// Keeps value * 10 from overflowing int64 while parsing.
constexpr std::size_t kMaxDigits = 18;

// Drawing fixtures use local labels; community peers use key fingerprints.
bool drawingIdentity(const std::string& value) {
    return !value.empty() &&
           (value.size() <= 20 ||
            (value.size() == 64 && std::all_of(value.begin(), value.end(), [](char c) {
                 return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
             })));
}

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

bool parseU32(const std::string& text, std::uint32_t& out) {
    std::int64_t value = 0;
    if (text.empty() || text[0] == '-' || text[0] == '+' || !parseInt64(text, value) ||
        value > 0xFFFFFFFFLL) {
        return false;
    }
    out = static_cast<std::uint32_t>(value);
    return true;
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
    if (size == 0 || size > kMaxDrawingFrameSize) {
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
    switch (message.type) {
        case MsgType::ScreenDrawState:
        case MsgType::ScreenDraw:
        case MsgType::ScreenDrawWatch:
        case MsgType::ScreenDrawErase:
        case MsgType::ScreenLaser:
            break;
        default:
            return DecodeStatus::Malformed;
    }

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

bool parseScreenDraw(const Message& message, bool fromOwner, ScreenDraw& out) {
    const std::size_t legacySize = fromOwner ? 9 : 8;
    if (message.type != MsgType::ScreenDraw ||
        (message.fields.size() != legacySize && message.fields.size() != legacySize + 2)) {
        return false;
    }
    ScreenDraw draw;
    draw.sharer = message.fields[0];
    if (!drawingIdentity(draw.sharer) || !parseInt64(message.fields[1], draw.epoch) ||
        draw.epoch <= 0)
        return false;
    const std::size_t idIndex = fromOwner ? 3 : 2;
    if (!parseU32(message.fields[idIndex], draw.id))
        return false;
    const std::size_t start = idIndex + 1;
    if (fromOwner)
        draw.author = message.fields[2];
    int* values[] = {&draw.x1, &draw.y1, &draw.x2, &draw.y2, &draw.color};
    for (std::size_t i = 0; i < 5; ++i) {
        std::uint32_t value = 0;
        if (!parseU32(message.fields[start + i], value) ||
            value > static_cast<std::uint32_t>(i == 4 ? kPenColorCount - 1 : kDrawCoordinateMax)) {
            return false;
        }
        *values[i] = static_cast<int>(value);
    }
    if (fromOwner && (!drawingIdentity(draw.author)))
        return false;
    if (message.fields.size() == legacySize + 2) {
        std::int64_t rgb = 0;
        if (!parseU32(message.fields[legacySize], draw.stroke) ||
            !parseInt64(message.fields[legacySize + 1], rgb) || rgb < -1 || rgb > 0xffffff)
            return false;
        draw.rgb = static_cast<int>(rgb);
    }
    out = std::move(draw);
    return true;
}

Message screenDrawMessage(const ScreenDraw& draw, bool fromOwner) {
    Message message{MsgType::ScreenDraw, {draw.sharer, std::to_string(draw.epoch)}};
    if (fromOwner)
        message.fields.push_back(draw.author);
    message.fields.push_back(std::to_string(draw.id));
    for (int value : {draw.x1, draw.y1, draw.x2, draw.y2, draw.color}) {
        message.fields.push_back(std::to_string(value));
    }
    if (draw.stroke != 0 || draw.rgb >= 0) {
        message.fields.push_back(std::to_string(draw.stroke));
        message.fields.push_back(std::to_string(draw.rgb));
    }
    return message;
}

bool parseScreenErase(const Message& message, ScreenErase& out) {
    if (message.type != MsgType::ScreenDrawErase || message.fields.size() < 4 ||
        message.fields.size() > 3 + kMaxEraseStrokes)
        return false;
    ScreenErase erase;
    erase.sharer = message.fields[0];
    erase.author = message.fields[2];
    if (!drawingIdentity(erase.sharer) || !drawingIdentity(erase.author) ||
        !parseInt64(message.fields[1], erase.epoch) || erase.epoch <= 0)
        return false;
    for (std::size_t i = 3; i < message.fields.size(); ++i) {
        std::uint32_t id = 0;
        if (!parseU32(message.fields[i], id))
            return false;
        erase.strokes.push_back(id);
    }
    out = std::move(erase);
    return true;
}

Message screenEraseMessage(const ScreenErase& erase) {
    Message message{MsgType::ScreenDrawErase,
                    {erase.sharer, std::to_string(erase.epoch), erase.author}};
    for (const auto id : erase.strokes)
        message.fields.push_back(std::to_string(id));
    return message;
}

bool parseScreenLaser(const Message& message, bool fromOwner, ScreenLaser& out) {
    if (message.type != MsgType::ScreenLaser || message.fields.size() != (fromOwner ? 8 : 7))
        return false;
    ScreenLaser laser;
    laser.sharer = message.fields[0];
    if (!drawingIdentity(laser.sharer) || !parseInt64(message.fields[1], laser.epoch) ||
        laser.epoch <= 0)
        return false;
    if (fromOwner) {
        laser.author = message.fields[2];
        if (!drawingIdentity(laser.author))
            return false;
    }
    const std::size_t start = fromOwner ? 3 : 2;
    std::uint32_t x = 0, y = 0, rgb = 0;
    if (!parseU32(message.fields[start], laser.id) || laser.id == 0 ||
        !parseU32(message.fields[start + 1], x) || x > kDrawCoordinateMax ||
        !parseU32(message.fields[start + 2], y) || y > kDrawCoordinateMax ||
        !parseU32(message.fields[start + 3], rgb) || rgb > 0xffffff ||
        (message.fields[start + 4] != "0" && message.fields[start + 4] != "1"))
        return false;
    laser.x = static_cast<int>(x);
    laser.y = static_cast<int>(y);
    laser.rgb = static_cast<int>(rgb);
    laser.visible = message.fields[start + 4] == "1";
    out = std::move(laser);
    return true;
}

Message screenLaserMessage(const ScreenLaser& laser, bool fromOwner) {
    Message message{MsgType::ScreenLaser, {laser.sharer, std::to_string(laser.epoch)}};
    if (fromOwner)
        message.fields.push_back(laser.author);
    for (const auto& value :
         {std::to_string(laser.id), std::to_string(laser.x), std::to_string(laser.y),
          std::to_string(laser.rgb), std::string(laser.visible ? "1" : "0")})
        message.fields.push_back(value);
    return message;
}

} // namespace chat
