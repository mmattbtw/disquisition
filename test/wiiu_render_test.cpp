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
    std::size_t inkRight(int left) const {
        std::size_t ink = 0;
        for (int y = 0; y < height; ++y)
            for (int x = left; x < width; ++x) ink += pixels[y * width + x] != wiiu::background;
        return ink;
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
    const std::vector<wiiu::Rect> layout = {wiiu::tabRect(0), wiiu::tabRect(3), wiiu::settingRect(4),
        wiiu::composeRect(), wiiu::actionRect(0), wiiu::actionRect(2), wiiu::voiceRect(2)};
    for (std::size_t index = 0; index < 52 + layout.size(); ++index) {
        Image image;
        // All keys, the first/last tab, and full-width or rightmost buttons.
        const wiiu::Rect rect = index < 52 ? editor.keyRect(int(index)) : layout[index - 52];
        const auto label = index < 52 ? editor.key(int(index)) :
            index == 52 ? std::string("Communities") : std::string(100, 'W');
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

void touchTargetsNeverOverlap() {
    for (int y = 0; y < 480; y += 2) for (int x = 0; x < 854; x += 2) {
        int communities = 0, settings = 0;
        for (int i = 0; i < 4; ++i) {
            communities += wiiu::tabRect(i).contains(x, y);
            settings += wiiu::tabRect(i).contains(x, y);
        }
        for (int i = 0; i < 3; ++i) communities += wiiu::actionRect(i).contains(x, y) + wiiu::voiceRect(i).contains(x, y);
        for (int row = 0; row < wiiu::contentRows; ++row)
            communities += wiiu::communityRect(row).contains(x, y) + wiiu::channelRect(row).contains(x, y);
        for (int i = 0; i < 5; ++i) settings += wiiu::settingRect(i).contains(x, y);
        CHECK(communities <= 1 && settings <= 1);
        CHECK(!wiiu::composeRect().contains(x, y) || !wiiu::voiceRect(0).contains(x, y));
    }
    CHECK(wiiu::channelRect(0).column + wiiu::channelRect(0).columns <= 53);
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

console::MemberView member(const char* id, const char* name, bool online, const char* voice = "") {
    console::MemberView result;
    result.id = id; result.name = name; result.online = online; result.voiceChannel = voice;
    return result;
}

console::View sampleView() {
    console::View view;
    view.started = view.relayConnected = true;
    view.status = "Connected through the relay.";
    view.name = "wiiu";
    console::CommunityView home;
    home.id = "c1";
    home.name = "Software Engineering";
    home.channels = {{"t1", "general", "text", "Anything goes", false},
                     {"t2", "homebrew", "text", "", true},
                     {"v1", "Lounge", "voice", "", false},
                     {"v2", "Pairing", "voice", "", false}};
    home.members = {member("p1", "wiiu", true, "v1"), member("p2", "matt", true, "v1"),
                    member("p3", "jack", true), member("p4", "cameron", false)};
    home.members[0].self = true;
    home.members[1].speaking = true;
    home.members[2].muted = true;
    console::CommunityView games;
    games.id = "c2";
    games.name = "Game night with an unusually long community name";
    games.unread = true;
    console::CommunityView pending;
    pending.id = "c3";
    pending.name = "Joining...";
    pending.pending = true;
    view.communities = {home, games, pending};
    view.community = "c1";
    view.channel = "t1";
    const auto line = [](const char* sender, const char* body, const char* color) {
        console::ChatLine result;
        result.sender = sender; result.body = body; result.color = color;
        return result;
    };
    view.messages = {line("matt", "anyone around for voice?", "pink"),
                     line("wiiu", "on the GamePad now", "mint"),
                     line("jack", "caf\xc3\xa9 later. This message is long enough to wrap onto a "
                          "second line on the GamePad screen.", "#87afff"),
                     line("matt", "[sent an image]", "pink")};
    view.messages[1].local = true;
    view.voiceCommunity = "c1";
    view.voiceChannel = "v1";
    view.transmitting = true;
    return view;
}

void checkFrame(const Image& gamepad, const Image& tv) {
    // Every screen keeps a fully rendered footer and a separated header.
    CHECK(gamepad.count(wiiu::foreground, 456, 476) > 0);
    CHECK(gamepad.count(wiiu::foreground, 476, 480) == 0);
    CHECK(gamepad.count(wiiu::background, 44, 48) == std::size_t(4 * 854));
    // Nothing is drawn past the last text column on either screen.
    CHECK(gamepad.inkRight(53 * 16) == 0);
    CHECK(tv.inkRight(79 * 16) == 0);
}

void renderScreens(const std::filesystem::path& output) {
    const auto view = sampleView();
    wiiu::Settings settings;
    wiiu::Editor editor;
    std::string notice = "Voice joined. Hold ZR to talk.";
    const char* names[] = {"communities", "channels", "chat", "members", "settings", "keyboard", "offline"};
    for (int i = 0; i < 7; ++i) {
        console::View offline;
        wiiu::Scene scene{i == 6 ? offline : view, settings, editor, notice};
        scene.voice = true;
        scene.setting = 4;
        if (i == 0) scene.tab = wiiu::Tab::Communities;
        if (i == 1) {
            scene.pane = wiiu::Pane::Channels;
            scene.channel = 2;
        }
        if (i >= 2 && i <= 4) scene.tab = static_cast<wiiu::Tab>(i - 1);
        if (i == 5) {
            notice.clear();
            editor.open("Invite link", "disquisition://join/" + std::string(180, 'x'), 512);
        }
        if (i == 6) { editor.active = false; scene.voice = false; }
        Image tv(1280, 720), gamepad;
        wiiu::render(scene, tv, gamepad);
        checkFrame(gamepad, tv);
        if (i == 1) {
            // The channel cursor bar sits on the Lounge row.
            CHECK(gamepad.pixels[(6 * 24 + 12) * 854 + 23 * 16 - 4] == wiiu::border);
            CHECK(gamepad.pixels[(4 * 24 + 12) * 854 + 23 * 16 - 4] != wiiu::border);
        }
        if (!output.empty()) {
            gamepad.save(output / (std::string(names[i]) + ".ppm"));
            if (i == 2) tv.save(output / "tv.ppm");
        }
    }
}
}

int main(int argc, char** argv) {
    labelsStayInsideTheirTouchableButtons();
    touchTargetsNeverOverlap();
    footerAndRightmostCharacterRenderCompletely();
    printableCharactersAndDisplaySanitization();
    const std::filesystem::path output = argc > 1 ? argv[1] : "";
    if (!output.empty()) std::filesystem::create_directories(output);
    renderScreens(output);
    std::puts("wiiu_render_test: ok");
}
