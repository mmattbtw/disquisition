#include "ui.h"
#include "settings.h"

namespace wiiu {

bool Rect::contains(int x, int y) const {
    return x >= column * cellWidth && x < (column + columns) * cellWidth &&
        y >= row * cellHeight && y < (row + rows) * cellHeight;
}

std::size_t firstVisible(std::size_t cursor, std::size_t count, std::size_t rows) {
    if (count <= rows) return 0;
    const auto first = cursor >= rows ? cursor - rows + 1 : 0;
    return first + rows > count ? count - rows : first;
}

std::string displayText(const std::string& utf8) {
    std::string result;
    for (unsigned char c : utf8) {
        if (c >= 32 && c < 127) result.push_back(char(c));
        else if (c >= 0xc0) result.push_back('?');
        else if (c < 32) result.push_back(' ');
    }
    return result;
}

std::vector<TranscriptLine> chatLines(const std::vector<console::ChatLine>& messages, std::size_t width) {
    std::vector<TranscriptLine> lines;
    if (width == 0) return lines;
    for (const auto& message : messages) {
        std::string text = displayText(message.sender) + ": " + displayText(message.body);
        if (message.undelivered) text += "  (not delivered)";
        while (text.size() > width) {
            auto end = text.rfind(' ', width);
            if (end == std::string::npos || end == 0) end = width;
            lines.push_back({text.substr(0, end), message.color});
            text.erase(0, end);
            if (!text.empty() && text.front() == ' ') text.erase(0, 1);
        }
        lines.push_back({std::move(text), message.color});
    }
    return lines;
}

namespace {
std::uint32_t rgb(unsigned red, unsigned green, unsigned blue) {
    return (red << 24) | (green << 16) | (blue << 8);
}
// Mirrors src/desktop/chat_color.cpp.
std::uint32_t xterm(int index) {
    static constexpr unsigned char basic[16][3] = {
        {0, 0, 0},       {205, 49, 49},   {13, 188, 121}, {229, 229, 16},
        {36, 114, 200},  {188, 63, 188},  {17, 168, 205}, {229, 229, 229},
        {102, 102, 102}, {241, 76, 76},   {35, 209, 139}, {245, 245, 67},
        {59, 142, 234},  {214, 112, 214}, {41, 184, 219}, {255, 255, 255}};
    if (index < 16) return rgb(basic[index][0], basic[index][1], basic[index][2]);
    if (index < 232) {
        static constexpr unsigned levels[] = {0, 95, 135, 175, 215, 255};
        const int value = index - 16;
        return rgb(levels[value / 36], levels[value / 6 % 6], levels[value % 6]);
    }
    const unsigned gray = 8 + unsigned(index - 232) * 10;
    return rgb(gray, gray, gray);
}
}

std::uint32_t messageColor(const std::string& color) {
    static constexpr int named[kColorCount] = {211, 121, 229, 111, 183, 159, 216};
    for (std::size_t i = 0; i < kColorCount; ++i) if (color == kColors[i]) return xterm(named[i]);
    if (color.size() == 7 && color[0] == '#' && validColor(color))
        return std::uint32_t(std::stoul(color.substr(1), nullptr, 16)) << 8;
    if (!color.empty() && color.size() <= 3 && color.find_first_not_of("0123456789") == std::string::npos) {
        const int index = std::stoi(color);
        if (index <= 255) return xterm(index);
    }
    return xterm(121);
}

const console::CommunityView* selectedCommunity(const console::View& view) {
    for (const auto& community : view.communities)
        if (!community.pending && community.id == view.community) return &community;
    return nullptr;
}

std::string channelName(const console::CommunityView& community, const std::string& channel) {
    for (const auto& entry : community.channels) if (entry.id == channel) return entry.name;
    return "voice";
}

void Editor::open(const std::string& heading, const std::string& value, std::size_t limit) {
    active = true; shifted = false; selected = 0; title = heading;
    text = value.substr(0, limit); cursor_ = text.size(); limit_ = limit;
}
void Editor::move(int horizontal, int vertical) {
    int row = selected < 48 ? selected / 12 : 4;
    int column = selected < 48 ? selected % 12 : (selected - 48) * 3;
    row = (row + vertical + 5) % 5;
    if (row == 4) selected = 48 + ((column / 3 + horizontal + 4) % 4);
    else selected = row * 12 + (column + horizontal + 12) % 12;
}
Rect Editor::keyRect(int index) const {
    if (index < 48) return {2 + (index % 12) * 4, 9 + (index / 12) * 2, 4, 2};
    return {2 + (index - 48) * 12, 17, 12, 2};
}
bool Editor::touch(int x, int y) {
    for (int i = 0; i < 52; ++i) if (keyRect(i).contains(x, y)) { selected = i; return true; }
    return false;
}
std::string Editor::key(int index) const {
    static constexpr char lower[] = "1234567890-=qwertyuiop[]asdfghjkl;'\\zxcvbnm,./` ";
    static constexpr char upper[] = "!@#$%^&*()_+QWERTYUIOP{}ASDFGHJKL:\"|ZXCVBNM<>?~ ";
    static_assert(sizeof(lower) == 49 && sizeof(upper) == 49);
    if (index < 48) return std::string(1, shifted ? upper[index] : lower[index]);
    switch (index) {
    case 48: return shifted ? "lowercase" : "Shift";
    case 49: return "Space";
    case 50: return "Delete";
    default: return "Done";
    }
}
void Editor::insert(char value) {
    if (text.size() >= limit_) return;
    text.insert(cursor_, 1, value); ++cursor_;
}
Editor::Result Editor::press() {
    if (selected < 48) insert(key(selected)[0]);
    else if (selected == 48) shifted = !shifted;
    else if (selected == 49) insert(' ');
    else if (selected == 50) backspace();
    else return Accepted;
    return Editing;
}
void Editor::backspace() {
    if (cursor_ > 0) { text.erase(cursor_ - 1, 1); --cursor_; }
}
void Editor::moveCursor(int direction) {
    if (direction < 0 && cursor_ > 0) --cursor_;
    else if (direction > 0 && cursor_ < text.size()) ++cursor_;
}

} // namespace wiiu
