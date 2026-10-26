#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

// Drawing packets travel inside authenticated community peer channels. The
// numeric values preserve compatibility with existing screen-drawing sessions.
namespace chat {
enum class MsgType : std::uint8_t {
    ScreenDrawState = 27,
    ScreenDraw = 28,
    ScreenDrawWatch = 29,
    ScreenDrawErase = 32,
    ScreenLaser = 33,
};
struct Message {
    MsgType type = MsgType::ScreenDrawState;
    std::vector<std::string> fields;
};
constexpr std::uint32_t kMaxDrawingFrameSize = 16 * 1024;
std::string encode(const Message& message);
enum class DecodeStatus { Ok, Incomplete, Malformed };
DecodeStatus decode(std::string& buffer, Message& out);
bool parseInt64(const std::string& text, std::int64_t& out);

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
    int rgb = -1;             // 24-bit RGB; -1 uses the legacy palette
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

} // namespace chat
