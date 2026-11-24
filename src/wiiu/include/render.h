#pragma once

#include "settings.h"
#include "ui.h"

namespace wiiu {

constexpr std::uint32_t background = 0x101C2600;
constexpr std::uint32_t foreground = 0xE8EDF100;
constexpr std::uint32_t accent = 0xA8DBC700;
constexpr std::uint32_t border = 0x43556300;
constexpr std::uint32_t dim = 0x8A97A300;

// Only pixel output differs between the console and the host preview. Text,
// borders and hit targets all use the app's grid, never OSScreen font metrics.
class Canvas {
public:
    Canvas(int width, int height) : width_(width), height_(height) {}
    virtual ~Canvas() = default;
    void pixel(int x, int y, std::uint32_t color);
    void text(int column, int row, const std::string& value, std::size_t columns = 51,
              std::uint32_t color = foreground);
    void button(Rect rect, const std::string& label, bool selected = false);
    // A list row's cursor bar, slightly wider than its text.
    void highlight(Rect rect);
    void fill(int left, int top, int right, int bottom, std::uint32_t color);
    void speaker(int column, int row, std::uint32_t color);
private:
    int width_, height_;
    virtual void putPixel(int x, int y, std::uint32_t color) = 0;
    void glyph(int x, int y, const unsigned char* rows, std::uint32_t color);
    void textAt(int x, int y, const std::string& value, std::size_t columns, std::uint32_t color);
};

enum class Tab { Communities, Chat, Members, Settings };
enum class Pane { Communities, Channels };
struct Scene {
    const console::View& view;
    const Settings& settings;
    const Editor& editor;
    const std::string& notice;
    Tab tab = Tab::Communities;
    Pane pane = Pane::Communities;
    std::size_t community = 0, channel = 0; // list cursors
    std::size_t scroll = 0, memberScroll = 0;
    int setting = 0;
    bool voice = false, muted = false, deafened = false, confirmLeave = false;
};
void render(const Scene& scene, Canvas& tv, Canvas& gamepad);

} // namespace wiiu
