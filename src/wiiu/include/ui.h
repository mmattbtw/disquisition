#pragma once

#include "session.h"
#include "settings.h"
#include <cstddef>
#include <string>
#include <vector>

namespace wiiu {

// Character-cell rectangles match OSScreen's 16 x 24 layout on the GamePad.
struct Rect {
    int column, row, columns, rows;
    bool contains(int x, int y) const;
};

struct ChatLine { std::string text, color; };
std::string displayText(const std::string& utf8);
std::vector<ChatLine> chatLines(const std::vector<chat::ChatPayload>& messages, std::size_t width);
std::uint32_t messageColor(const std::string& color);

class Editor {
public:
    bool active = false;
    bool shifted = false;
    int selected = 0;
    std::string title, text;
    void open(const std::string& heading, const std::string& value, std::size_t limit);
    void move(int horizontal, int vertical);
    bool touch(int x, int y); // true when a key/button was selected
    enum Result { Editing, Accepted };
    Result press();
    void backspace();
    void moveCursor(int direction);
    std::size_t cursor() const { return cursor_; }
    std::string key(int index) const;
    Rect keyRect(int index) const;
private:
    std::size_t limit_ = 0, cursor_ = 0;
    void insert(char value);
};

} // namespace wiiu
