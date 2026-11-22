#pragma once

#include "console/session.h"
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace wiiu {

// Text, borders and touch targets share this app-controlled pixel grid.
constexpr int cellWidth = 16, cellHeight = 24;
struct Rect {
    int column, row, columns, rows;
    bool contains(int x, int y) const;
};

// GamePad layout (53 x 20 cells), shared by drawing and touch.
constexpr int contentRow = 4, contentRows = 10;
inline Rect tabRect(int index) { return {1 + index * 13, 2, 13, 2}; }
inline Rect settingRect(int index) { return {1, 4 + index * 2, 51, 2}; }
inline Rect communityRect(int row) { return {1, contentRow + row, 20, 1}; }
inline Rect channelRect(int row) { return {23, contentRow + row, 30, 1}; }
inline Rect composeRect() { return {1, 14, 51, 2}; }
inline Rect actionRect(int index) { return {1 + index * 17, 14, 17, 2}; }
inline Rect voiceRect(int index) { return {1 + index * 17, 16, 17, 2}; }
// The first row of a list scrolled so that cursor stays visible.
std::size_t firstVisible(std::size_t cursor, std::size_t count, std::size_t rows);

struct TranscriptLine { std::string text, color; };
std::string displayText(const std::string& utf8);
std::vector<TranscriptLine> chatLines(const std::vector<console::ChatLine>& messages, std::size_t width);
// The desktop's rendering of a chat color, as OSScreen RGBX.
std::uint32_t messageColor(const std::string& color);

const console::CommunityView* selectedCommunity(const console::View& view);
std::string channelName(const console::CommunityView& community, const std::string& channel);

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
