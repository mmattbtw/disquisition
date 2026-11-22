#include "check.h"
#include "lifecycle.h"
#include "microphone_frames.h"
#include "settings.h"
#include "storage.h"
#include "ui.h"

#include <filesystem>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <unistd.h>

namespace {
namespace fs = std::filesystem;

int menuRequests = 0, launcherStops = 0;
void launchMenu() { ++menuRequests; }
void stopLauncher() { ++launcherStops; }

fs::path temporaryFolder() {
    char pattern[] = "/tmp/disquisition-wiiu-XXXXXX";
    const char* folder = mkdtemp(pattern);
    CHECK(folder != nullptr);
    return folder;
}
void writeFile(const fs::path& path, const std::string& text) {
    std::ofstream file(path, std::ios::binary);
    file << text;
    CHECK(file.good());
}

void aromaExitRequestsMenuOnceWithoutStoppingProcuiEarly() {
    menuRequests = launcherStops = 0;
    wiiu::ExitRequest exit;
    CHECK(!exit.requested());
    exit.request(0x0005000012345678ULL, launchMenu, stopLauncher);
    CHECK(exit.requested());
    CHECK(menuRequests == 1 && launcherStops == 0);
    exit.request(0x0005000012345678ULL, launchMenu, stopLauncher);
    CHECK(menuRequests == 1 && launcherStops == 0);
}

void legacyLauncherExitKeepsLibwhbRelaunchBehavior() {
    for (const auto title : {0x0005000013374842ULL, 0x000500101004A000ULL,
                            0x000500101004A100ULL, 0x000500101004A200ULL}) {
        menuRequests = launcherStops = 0;
        wiiu::ExitRequest exit;
        exit.request(title, launchMenu, stopLauncher);
        CHECK(exit.requested());
        CHECK(menuRequests == 0 && launcherStops == 1);
        exit.request(title, launchMenu, stopLauncher);
        CHECK(menuRequests == 0 && launcherStops == 1);
    }
}

void pcmUsesLittleEndianOnEveryHost() {
    console::Samples samples{};
    samples[0] = 0x1234; samples[1] = -2;
    samples[2] = std::numeric_limits<std::int16_t>::min();
    samples[3] = std::numeric_limits<std::int16_t>::max();
    const auto pcm = console::encodePcm(samples);
    CHECK(pcm.size() == 640);
    CHECK(pcm.substr(0, 8) == std::string("\x34\x12\xfe\xff\x00\x80\xff\x7f", 8));
    console::Samples decoded{};
    CHECK(console::decodePcm(pcm, decoded));
    CHECK(decoded == samples);
    CHECK(!console::decodePcm(pcm.substr(1), decoded));
}

void microphonePairsAcrossChunksAndDropsMutedPartialFrame() {
    wiiu::MicrophoneFrames capture;
    console::Samples frame{};
    for (int i = 0; i < 639; ++i) CHECK(!capture.push(i % 2 ? 32767 : 32765, frame));
    CHECK(capture.push(32767, frame));
    for (const auto sample : frame) CHECK(sample == 32766);
    for (int i = 0; i < 317; ++i) CHECK(!capture.push(12000, frame));
    capture.reset();
    for (int i = 0; i < 639; ++i) CHECK(!capture.push(-32768, frame));
    CHECK(capture.push(-32768, frame));
    for (const auto sample : frame) CHECK(sample == -32768);
}

void settingsRejectInvalidInputAndRecoverBackup() {
    auto settings = wiiu::parseSettings(
        "name= \nrelay=bad/path\ncolor=250\npush_to_talk=garbage\nhost=relay.mmatt.net\nport=9000\n");
    CHECK(settings.name == "wiiu" && settings.relay == "relay.disquisition.app");
    CHECK(settings.color == "mint" && settings.pushToTalk);
    settings = wiiu::parseSettings("name=Living room\nrelay=192.168.1.20:4444\ncolor=#A1B2C3\npush_to_talk=0\n");
    CHECK(settings.name == "Living room" && settings.relay == "192.168.1.20:4444");
    CHECK(settings.color == "#A1B2C3" && !settings.pushToTalk);
    CHECK(wiiu::parseSettings(wiiu::encodeSettings(settings)).relay == settings.relay);
    CHECK(wiiu::validName(std::string(64, 'n')) && !wiiu::validName(std::string(65, 'n')));
    CHECK(!wiiu::validName("") && !wiiu::validName("   ") && !wiiu::validName("a\nb"));
    CHECK(wiiu::validRelay("relay.disquisition.app") && wiiu::validRelay("10.0.0.2:3333"));
    CHECK(!wiiu::validRelay("") && !wiiu::validRelay("example.com:70000"));
    CHECK(!wiiu::validRelay("tcp://example.com") && !wiiu::validRelay("relay.example\nname=evil"));
    CHECK(!wiiu::validRelay("two words"));
    for (const auto* color : wiiu::kColors) CHECK(wiiu::validColor(color));
    CHECK(wiiu::validColor("0") && wiiu::validColor("255") && wiiu::validColor("#ffffff"));
    CHECK(!wiiu::validColor("1") && !wiiu::validColor("256") && !wiiu::validColor("#fffff"));
    CHECK(!wiiu::validColor("orange") && !wiiu::validColor(""));

    const auto root = temporaryFolder();
    const auto folder = (root / "wiiu/apps/disquisition").string();
    std::string notice;
    bool saved = true;
    CHECK(wiiu::loadSettings(folder, notice, saved).name == "wiiu" && !saved);
    wiiu::Settings stored;
    stored.name = "living room";
    CHECK(wiiu::saveSettings(folder, stored, notice));
    stored.name = "new name";
    CHECK(wiiu::saveSettings(folder, stored, notice));
    CHECK(wiiu::loadSettings(folder, notice, saved).name == "new name" && saved);
    fs::remove(folder + "/settings.cfg");
    CHECK(wiiu::loadSettings(folder, notice, saved).name == "living room" && saved);
    CHECK(notice == "Recovered settings from SD backup.");
    CHECK(!wiiu::saveSettings("", stored, notice));
    fs::remove_all(root);
}

void inviteFileUsesTheTrimmedFirstLine() {
    const auto root = temporaryFolder();
    const auto path = (root / "invite.txt").string();
    std::string link, notice;
    CHECK(!wiiu::readInvite(path, link, notice) && !notice.empty());
    writeFile(path, "\xEF\xBB\xBF\r\n  disquisition://join/abc123  \r\nsecond line\n");
    CHECK(wiiu::readInvite(path, link, notice) && link == "disquisition://join/abc123");
    writeFile(path, " \n\t\n");
    CHECK(!wiiu::readInvite(path, link, notice) && notice == "invite.txt is empty.");
    CHECK(!wiiu::readInvite("", link, notice));
    fs::remove_all(root);
}

void storageReplacesFilesAndRecoversInterruptedWrites() {
    const auto root = temporaryFolder();
    const auto folder = root / "wiiu/apps/disquisition/community";
    wiiu::SdStorage storage(folder.string());
    std::string bytes;
    CHECK(!storage.read("identity.key", bytes, 32));
    CHECK(storage.list(".community.json").empty());
    CHECK(fs::is_directory(folder));
    storage.write("identity.key", std::string(32, 'k'));
    storage.write("a.community.json", "first");
    storage.write("a.community.json", "second");
    storage.write("pending.json", "[]");
    CHECK(storage.read("a.community.json", bytes, 6) && bytes == "second");
    CHECK(!fs::exists(folder / "a.community.json.tmp") && !fs::exists(folder / "a.community.json.bak"));
    bool threw = false;
    try { storage.read("a.community.json", bytes, 5); } catch (const std::runtime_error&) { threw = true; }
    CHECK(threw && bytes == "second");
    // A replacement interrupted after moving the old copy aside.
    fs::rename(folder / "a.community.json", folder / "a.community.json.bak");
    writeFile(folder / "b.community.json.tmp", "partial");
    const auto names = storage.list(".community.json");
    CHECK(names == std::vector<std::string>{"a.community.json"});
    CHECK(storage.read("a.community.json", bytes, 64) && bytes == "second");
    storage.write("a.community.json", "third");
    CHECK(storage.read("a.community.json", bytes, 64) && bytes == "third");
    CHECK(!fs::exists(folder / "a.community.json.bak"));
    storage.remove("a.community.json");
    storage.remove("missing.community.json");
    CHECK(!storage.read("a.community.json", bytes, 64));
    CHECK(storage.list(".community.json").empty());
    CHECK(storage.read("identity.key", bytes, 32) && bytes == std::string(32, 'k'));
    // Unreadable is not missing: it must never look like a fresh install.
    fs::create_directory(folder / "broken.json");
    threw = false;
    try { storage.read("broken.json", bytes, 64); } catch (const std::runtime_error&) { threw = true; }
    CHECK(threw);
    fs::remove_all(root);
}

void keyboardCoversPrintableAsciiAndPreservesDraftCursor() {
    wiiu::Editor editor;
    editor.open("message", "", 500);
    std::string characters;
    for (bool shifted : {false, true}) {
        editor.shifted = shifted;
        for (int i = 0; i < 48; ++i) characters += editor.key(i);
    }
    for (char c = 32; c < 127; ++c) CHECK(characters.find(c) != std::string::npos);
    editor.open("message", "hi", 3);
    editor.moveCursor(-1);
    editor.selected = 0;
    editor.press();
    CHECK(editor.text == "h1i");
    editor.press(); CHECK(editor.text == "h1i");
    editor.backspace(); CHECK(editor.text == "hi");
    CHECK(editor.touch(2 * 16, 9 * 24));
    CHECK(editor.selected == 0);
    CHECK(!editor.touch(1, 1));
    editor.selected = 51; CHECK(editor.press() == wiiu::Editor::Accepted);
    // Invite links are typed in full.
    editor.open("Invite link", "", 512);
    editor.selected = 20;
    for (int i = 0; i < 600; ++i) editor.press();
    CHECK(editor.text.size() == 512);
}

void transcriptWrapsLongWordsAndSanitizesOnlyDisplay() {
    const std::string utf8 = "caf\xc3\xa9";
    CHECK(wiiu::displayText(utf8) == "caf?");
    std::vector<console::ChatLine> messages(1);
    messages[0].sender = "u";
    messages[0].body = std::string(110, 'x');
    messages[0].color = "20";
    const auto lines = wiiu::chatLines(messages, 51);
    CHECK(lines.size() == 4);
    for (const auto& line : lines) CHECK(line.text.size() <= 51 && line.color == "20");
    CHECK(wiiu::chatLines(messages, 0).empty());
    messages[0].body = "hello";
    messages[0].undelivered = true;
    CHECK(wiiu::chatLines(messages, 51).front().text == "u: hello  (not delivered)");
}

void colorsMatchTheDesktop() {
    CHECK(wiiu::messageColor("mint") == 0x87FFAF00);
    CHECK(wiiu::messageColor("pink") == 0xFF87AF00);
    CHECK(wiiu::messageColor("#12aBcD") == 0x12ABCD00);
    CHECK(wiiu::messageColor("9") == 0xF14C4C00);
    CHECK(wiiu::messageColor("232") == 0x08080800);
    CHECK(wiiu::messageColor("garbage") == wiiu::messageColor("mint"));
}

void listsScrollToKeepTheCursorVisible() {
    CHECK(wiiu::firstVisible(0, 5, 10) == 0);
    CHECK(wiiu::firstVisible(9, 20, 10) == 0);
    CHECK(wiiu::firstVisible(10, 20, 10) == 1);
    CHECK(wiiu::firstVisible(19, 20, 10) == 10);
    CHECK(wiiu::firstVisible(25, 20, 10) == 10);
}
}

int main() {
    aromaExitRequestsMenuOnceWithoutStoppingProcuiEarly();
    legacyLauncherExitKeepsLibwhbRelaunchBehavior();
    pcmUsesLittleEndianOnEveryHost();
    microphonePairsAcrossChunksAndDropsMutedPartialFrame();
    settingsRejectInvalidInputAndRecoverBackup();
    inviteFileUsesTheTrimmedFirstLine();
    storageReplacesFilesAndRecoversInterruptedWrites();
    keyboardCoversPrintableAsciiAndPreservesDraftCursor();
    transcriptWrapsLongWordsAndSanitizesOnlyDisplay();
    colorsMatchTheDesktop();
    listsScrollToKeepTheCursorVisible();
    std::puts("wiiu_client_test: ok");
}
