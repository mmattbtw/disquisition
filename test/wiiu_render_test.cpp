#include "check.h"
#include "render.h"

#include <algorithm>
#include <filesystem>
#include <fstream>

namespace {
// A real pixel buffer consumes the same renderer as OSScreen. No emulated
// font API or duplicated layout can hide a console/host positioning mismatch.
class Image final : public wiiu::Canvas {
public:
    Image(int w = 854, int h = 480) : Canvas(w, h), width(w), height(h), pixels(w * h, wiiu::background) {}
    int width, height;
    std::vector<std::uint32_t> pixels;
    std::size_t count(std::uint32_t color, int top, int bottom) const {
        return std::count(pixels.begin() + top * width, pixels.begin() + bottom * width, color);
    }
    void save(const std::filesystem::path& path) const {
        std::ofstream file(path, std::ios::binary);
        file << "P6\n" << width << ' ' << height << "\n255\n";
        for (const auto color : pixels) {
            const char rgb[] = {char(color >> 24), char(color >> 16), char(color >> 8)};
            file.write(rgb, sizeof(rgb));
        }
        CHECK(file.good());
    }
private:
    void putPixel(int x, int y, std::uint32_t color) override {
        CHECK(x >= 0 && x < width && y >= 0 && y < height);
        pixels[y * width + x] = color;
    }
};

void labelsStayInsideTheirTouchableButtons() {
    wiiu::Editor editor;
    for (int index = 0; index < 55; ++index) {
        Image image;
        // Include first/last tab and a full-width long setting, plus all keys.
        const wiiu::Rect rect = index < 52 ? editor.keyRect(index) : index == 52 ?
            wiiu::Rect{1, 2, 17, 2} : index == 53 ? wiiu::Rect{35, 2, 17, 2} : wiiu::Rect{1, 14, 51, 2};
        const auto label = index < 52 ? editor.key(index) : std::string(100, 'W');
        image.button(rect, label);
        std::size_t ink = 0;
        for (int y = 0; y < image.height; ++y) for (int x = 0; x < image.width; ++x) {
            if (image.pixels[y * image.width + x] != wiiu::foreground) continue;
            ++ink;
            CHECK(rect.contains(x, y));
            CHECK(x > rect.column * 16 && x < (rect.column + rect.columns) * 16 - 3);
            CHECK(y > rect.row * 24 && y < (rect.row + rect.rows) * 24 - 3);
            // Labels need air above and below, rather than sitting on a border.
            CHECK(y >= rect.row * 24 + 8 && y < (rect.row + rect.rows) * 24 - 8);
        }
        CHECK(ink > 0 || label == " ");
    }
}

void footerAndRightmostCharacterRenderCompletely() {
    Image top, bottom;
    const std::string label(51, 'W');
    top.text(1, 0, label);
    bottom.text(1, 19, label);
    CHECK(top.count(wiiu::foreground, 0, 24) > 0);
    CHECK(top.count(wiiu::foreground, 0, 24) == bottom.count(wiiu::foreground, 456, 480));
    for (int y = 0; y < 24; ++y) for (int x = 0; x < top.width; ++x)
        CHECK(top.pixels[y * top.width + x] == bottom.pixels[(y + 456) * bottom.width + x]);
    CHECK(bottom.count(wiiu::foreground, 476, 480) == 0);
    Image last;
    last.text(51, 0, "W");
    Image first;
    first.text(1, 0, "W");
    CHECK(first.count(wiiu::foreground, 0, 24) == last.count(wiiu::foreground, 0, 24));
}

void printableCharactersAndDisplaySanitization() {
    for (char c = 33; c < 127; ++c) {
        Image image;
        image.text(0, 0, std::string(1, c));
        CHECK(image.count(wiiu::foreground, 0, 24) > 0);
    }
    Image raw, sanitized;
    raw.text(1, 0, "hi\n\t\xc3\xa9!");
    sanitized.text(1, 0, "hi  ?!");
    CHECK(raw.pixels == sanitized.pixels);
}

void renderScreens(const std::filesystem::path& output) {
    handheld::View view;
    view.connected = view.voice = true;
    view.status = "Joined as wiiu";
    view.messages = {{"wiiu", 1, "CX", "mint"}, {"matt", 2, "wow", "pink"},
        {"matt", 3, "crazy", "pink"}, {"matt", 4, "amazing", "pink"}};
    view.members["wiiu"].voice = true;
    view.members["matt"].voice = true;
    view.members["matt"].lastAudio = 900;
    wiiu::Settings settings;
    wiiu::Editor editor;
    std::string notice = "Voice joined. Hold ZR to talk.";
    wiiu::Scene scene{view, settings, editor, notice};
    scene.joining = scene.voiceWanted = true;
    scene.setting = 5;
    scene.now = 1000;
    const char* names[] = {"chat", "members", "settings", "keyboard"};
    for (int i = 0; i < 4; ++i) {
        Image tv(1280, 720), gamepad;
        if (i == 3) { notice.clear(); editor.open("Write a message", "Hello from the Wii U!", 2000); }
        else scene.tab = static_cast<wiiu::Tab>(i);
        wiiu::render(scene, tv, gamepad);
        // Every screen keeps a fully rendered footer and separated header.
        CHECK(gamepad.count(wiiu::foreground, 456, 476) > 0);
        CHECK(gamepad.count(wiiu::foreground, 476, 480) == 0);
        CHECK(gamepad.count(wiiu::foreground, 44, 48) == 0);
        if (!output.empty()) {
            gamepad.save(output / (std::string(names[i]) + ".ppm"));
            if (i == 0) tv.save(output / "tv.ppm");
        }
    }
}
}

int main(int argc, char** argv) {
    labelsStayInsideTheirTouchableButtons();
    footerAndRightmostCharacterRenderCompletely();
    printableCharactersAndDisplaySanitization();
    const std::filesystem::path output = argc > 1 ? argv[1] : "";
    if (!output.empty()) std::filesystem::create_directories(output);
    renderScreens(output);
    std::puts("wiiu_render_test: ok");
}
