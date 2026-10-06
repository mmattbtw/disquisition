#include "ui.h"
#include <algorithm>

namespace wiiu {

bool Rect::contains(int x, int y) const {
    return x >= column * 16 && x < (column + columns) * 16 &&
        y >= row * 24 && y < (row + rows) * 24;
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

std::vector<ChatLine> chatLines(const std::vector<chat::ChatPayload>& messages, std::size_t width) {
    std::vector<ChatLine> lines;
    if (width == 0) return lines;
    for (const auto& message : messages) {
        std::string text = displayText(message.sender) + ": " + displayText(message.body);
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

std::uint32_t messageColor(const std::string& color) {
    constexpr std::uint32_t named[] = {0xF0A5C900, 0xA8DBC700, 0xEFDBA100, 0xACBBEE00,
        0xC4ADE600, 0x94DBDF00, 0xEEBAA300};
    for (std::size_t i = 0; i < chat::kColorCount; ++i) if (color == chat::kColorNames[i]) return named[i];
    int index = 0;
    if (!chat::parseColorIndex(color, index)) return named[1];
    if (index >= 232) {
        const unsigned gray = 8 + (index - 232) * 10;
        return (gray << 24) | (gray << 16) | (gray << 8);
    }
    if (index >= 16) {
        constexpr unsigned level[] = {0, 95, 135, 175, 215, 255};
        index -= 16;
        return (level[index / 36] << 24) | (level[index / 6 % 6] << 16) | (level[index % 6] << 8);
    }
    constexpr std::uint32_t basic[] = {0x00000000, 0x80000000, 0x00800000, 0x80800000,
        0x00008000, 0x80008000, 0x00808000, 0xc0c0c000, 0x80808000, 0xff000000,
        0x00ff0000, 0xffff0000, 0x0000ff00, 0xff00ff00, 0x00ffff00, 0xffffff00};
    return basic[index];
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
