#pragma once

#include "ui.h"

namespace wiiu {

constexpr std::uint32_t background = 0x101C2600;
constexpr std::uint32_t foreground = 0xE8EDF100;
constexpr std::uint32_t accent = 0xA8DBC700;
constexpr std::uint32_t border = 0x43556300;

// Only pixel output differs between the console and the host preview. Text,
// borders and hit targets all use the app's grid, never OSScreen font metrics.
class Canvas {
public:
    Canvas(int width, int height) : width_(width), height_(height) {}
    virtual ~Canvas() = default;
    void pixel(int x, int y, std::uint32_t color);
    void text(int column, int row, const std::string& value, std::size_t columns = 51);
    void button(Rect rect, const std::string& label, bool selected = false);
private:
    int width_, height_;
    virtual void putPixel(int x, int y, std::uint32_t color) = 0;
    void textAt(int x, int y, const std::string& value, std::size_t columns, std::uint32_t color);
};

enum class Tab { Chat, Members, Settings };
struct Scene {
    const handheld::View& view;
    const Settings& settings;
    const Editor& editor;
    const std::string& notice;
    Tab tab = Tab::Chat;
    bool joining = false, voiceWanted = false, muted = false, deafened = false;
    std::size_t scroll = 0, memberScroll = 0;
    int setting = 0;
    std::uint64_t now = 0;
};
void render(const Scene& scene, Canvas& tv, Canvas& gamepad);

} // namespace wiiu
