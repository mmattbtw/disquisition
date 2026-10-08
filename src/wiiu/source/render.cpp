#include "render.h"
#include "font8x8_basic.h"

#include <algorithm>

namespace wiiu {

void Canvas::pixel(int x, int y, std::uint32_t color) {
    if (x >= 0 && x < width_ && y >= 0 && y < height_) putPixel(x, y, color);
}

void Canvas::textAt(int x, int y, const std::string& value, std::size_t columns,
                    std::uint32_t color) {
    const auto visible = displayText(value).substr(0, columns);
    for (const unsigned char c : visible) {
        for (int row = 0; row < 8; ++row) {
            for (int column = 0; column < 8; ++column) {
                if (!(font8x8_basic[c][row] & (1u << column))) continue;
                for (int dy = 0; dy < 2; ++dy)
                    for (int dx = 0; dx < 2; ++dx)
                        pixel(x + column * 2 + dx, y + row * 2 + dy, color);
            }
        }
        x += cellWidth;
    }
}

void Canvas::text(int column, int row, const std::string& value, std::size_t columns) {
    textAt(column * cellWidth, row * cellHeight + 4, value, columns, foreground);
}

void Canvas::button(Rect r, const std::string& label, bool selected) {
    const int left = r.column * cellWidth, right = (r.column + r.columns) * cellWidth - 3;
    const int top = r.row * cellHeight, bottom = (r.row + r.rows) * cellHeight - 3;
    const auto color = selected ? accent : border;
    for (int x = left; x <= right; ++x) { pixel(x, top, color); pixel(x, bottom, color); }
    for (int y = top; y <= bottom; ++y) { pixel(left, y, color); pixel(right, y, color); }
    // Center the 16-pixel glyph box inside the visible button, not at its top.
    textAt(left + cellWidth, top + (bottom - top + 1 - 16) / 2,
           label, std::max(0, r.columns - 2), selected ? accent : foreground);
}

namespace {
void transcript(Canvas& canvas, const Scene& scene, int columns, int first, int rows) {
    const auto lines = chatLines(scene.view.messages, columns - 2);
    const auto maximum = lines.size() > std::size_t(rows) ? lines.size() - rows : 0;
    const auto end = lines.size() - std::min(scene.scroll, maximum);
    const auto start = end > std::size_t(rows) ? end - rows : 0;
    for (std::size_t i = start; i < end; ++i) {
        const int row = first + int(i - start);
        canvas.text(1, row, lines[i].text, columns - 2);
        for (int y = row * cellHeight + 4; y < row * cellHeight + 20; ++y)
            canvas.pixel(5, y, messageColor(lines[i].color));
    }
    if (lines.empty()) canvas.text(1, first, "Messages appear here after joining.");
}

void editor(Canvas& canvas, const Scene& scene) {
    const auto& edit = scene.editor;
    canvas.text(1, 2, edit.title);
    canvas.text(1, 3, scene.notice);
    std::string preview = edit.text;
    preview.insert(edit.cursor(), "|");
    const std::size_t start = edit.cursor() > 180 ? edit.cursor() - 180 : 0;
    for (int row = 0; row < 4; ++row) {
        const auto index = start + row * 50;
        if (index < preview.size()) canvas.text(1, 4 + row, preview.substr(index, 50));
    }
    for (int i = 0; i < 52; ++i) canvas.button(edit.keyRect(i), edit.key(i), edit.selected == i);
    canvas.text(1, 19, "A key  X delete  B back  + done  L/R cursor  - exit");
}
}

void render(const Scene& scene, Canvas& tv, Canvas& gamepad) {
    tv.text(1, 0, "DISQUISITION / WII U", 78);
    tv.text(1, 1, scene.view.status, 78);
    transcript(tv, scene, 80, 3, 23);
    tv.text(1, 27, "Use the GamePad to write, view members, and change settings.", 78);
    tv.text(1, 28, scene.notice, 78);
    gamepad.text(1, 0, "DISQUISITION / WII U");
    gamepad.text(1, 1, scene.view.status);
    if (scene.editor.active) { editor(gamepad, scene); return; }
    const char* tabs[] = {"Chat", "Members", "Settings"};
    for (int i = 0; i < 3; ++i) gamepad.button({1 + i * 17, 2, 17, 2}, tabs[i], int(scene.tab) == i);
    if (scene.tab == Tab::Settings) {
        const auto& settings = scene.settings;
        const std::string fields[] = {"Relay: " + settings.host, "Port: " + std::to_string(settings.port),
            "Name: " + settings.name, "Color: " + settings.color,
            settings.pushToTalk ? "Microphone: hold ZR to talk" : "Microphone: open mic",
            scene.joining ? "Save & disconnect" : "Save & join relay"};
        for (int i = 0; i < 6; ++i) gamepad.button({1, 4 + i * 2, 51, 2}, fields[i], scene.setting == i);
        gamepad.text(1, 16, "Use a relay port, usually 3333.");
        gamepad.text(1, 17, "Text settings change while disconnected.");
    } else {
        if (scene.tab == Tab::Chat) transcript(gamepad, scene, 53, 4, 10);
        else {
            const auto maximum = scene.view.members.size() > 10 ? scene.view.members.size() - 10 : 0;
            const auto offset = std::min(scene.memberScroll, maximum);
            std::size_t index = 0;
            for (const auto& [name, member] : scene.view.members) {
                if (index++ < offset) continue;
                const int row = 4 + int(index - offset - 1);
                if (row >= 14) break;
                const std::string state = member.deafened ? "deafened" : member.muted ? "muted" :
                    member.lastAudio && scene.now - member.lastAudio < 600 ? "speaking" : member.voice ? "voice" : "text";
                gamepad.text(1, row, name + "  " + state);
            }
            if (scene.view.members.empty()) gamepad.text(1, 4, "Join to see the room's members.");
        }
        gamepad.button({1, 14, 51, 2}, scene.view.connected ? "A / tap to write a message" :
            scene.joining ? "Connecting... + cancels" : "A / tap to join relay");
        gamepad.button({1, 16, 17, 2}, scene.voiceWanted ? "Leave voice" : "Join voice", scene.voiceWanted);
        gamepad.button({18, 16, 17, 2}, scene.muted ? "Unmute" : "Mute", scene.muted);
        gamepad.button({35, 16, 17, 2}, scene.deafened ? "Undeafen" : "Deafen", scene.deafened);
    }
    gamepad.text(1, 18, scene.notice);
    gamepad.text(1, 19, "L/R tabs  + join/leave  ZR talk  - exit");
}

} // namespace wiiu
