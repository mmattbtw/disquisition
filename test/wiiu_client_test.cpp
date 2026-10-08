#include "check.h"
#include "microphone_frames.h"
#include "lifecycle.h"
#include "settings.h"
#include "ui.h"

#include <filesystem>
#include <fstream>
#include <limits>
#include <unistd.h>

namespace {

int menuRequests = 0, launcherStops = 0;
void launchMenu() { ++menuRequests; }
void stopLauncher() { ++launcherStops; }

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
    handheld::Samples samples{};
    samples[0] = 0x1234; samples[1] = -2;
    samples[2] = std::numeric_limits<std::int16_t>::min();
    samples[3] = std::numeric_limits<std::int16_t>::max();
    const auto pcm = handheld::encodePcm(samples);
    CHECK(pcm.size() == 640);
    CHECK(pcm.substr(0, 8) == std::string("\x34\x12\xfe\xff\x00\x80\xff\x7f", 8));
    handheld::Samples decoded{};
    CHECK(handheld::decodePcm(pcm, decoded));
    CHECK(decoded == samples);
    CHECK(!handheld::decodePcm(pcm.substr(1), decoded));
}

void microphonePairsAcrossChunksAndDropsMutedPartialFrame() {
    wiiu::MicrophoneFrames capture;
    handheld::Samples frame{};
    for (int i = 0; i < 639; ++i) CHECK(!capture.push(i % 2 ? 32767 : 32765, frame));
    CHECK(capture.push(32767, frame));
    for (const auto sample : frame) CHECK(sample == 32766);
    for (int i = 0; i < 317; ++i) CHECK(!capture.push(12000, frame));
    capture.reset();
    for (int i = 0; i < 639; ++i) CHECK(!capture.push(-32768, frame));
    CHECK(capture.push(-32768, frame));
    for (const auto sample : frame) CHECK(sample == -32768);
}

void voiceGatesCaptureAndReconnectLeavesVoiceOff() {
    handheld::Session session;
    session.receive({chat::MsgType::LoginOk, {"wiiu", "welcome"}}, 1);
    session.setVoice(true);
    session.controls(false, false, true, false);
    session.takeOutgoing();
    const std::string frame(640, '\0');
    session.capture(frame);
    CHECK(session.takeOutgoing().empty());
    session.controls(false, false, true, true);
    session.takeOutgoing();
    session.capture(frame);
    const auto sent = session.takeOutgoing();
    CHECK(sent.size() == 1 && sent[0].type == chat::MsgType::VoiceAudio);
    session.controls(false, true, false, true);
    session.takeOutgoing();
    session.capture(frame);
    CHECK(session.takeOutgoing().empty());
    session.disconnected("network lost");
    session.receive({chat::MsgType::LoginOk, {"wiiu-2", "welcome"}}, 2);
    CHECK(session.view.connected && !session.view.voice && !session.view.transmitting);
    CHECK(session.view.name == "wiiu-2");
}

void liveMessagesKeepArrivalOrderAndReconnectHistoryDoesNotReorderThem() {
    handheld::Session session;
    session.receive({chat::MsgType::LoginOk, {"wiiu", "welcome"}}, 1);
    session.receive({chat::MsgType::PeerChat, {"desktop", "500", "first", "mint"}}, 2);
    CHECK(session.sendChat("second", 100));
    session.receive({chat::MsgType::History, {"500", "desktop", "first", "mint"}}, 3);
    CHECK(session.view.messages.size() == 2);
    CHECK(session.view.messages[0].body == "first" && session.view.messages[1].body == "second");
}

void settingsRejectInvalidInputAndRecoverBackup() {
    const auto settings = wiiu::parseSettings("host=bad/path\nport=70000\nname= \ncolor=250\npush_to_talk=garbage\n");
    CHECK(settings.host == "relay.mmatt.net" && settings.port == 3333);
    CHECK(settings.name == "wiiu" && settings.color == "mint" && settings.pushToTalk);
    CHECK(!wiiu::validHost("example.com:3333") && !wiiu::validHost("127.0.0.1\nname=evil"));
    char pattern[] = "/tmp/disquisition-wiiu-settings-XXXXXX";
    const char* temporary = mkdtemp(pattern);
    CHECK(temporary != nullptr);
    const std::filesystem::path root(temporary);
    const auto folder = (root / "wiiu/apps/disquisition").string();
    std::string notice;
    wiiu::Settings saved;
    saved.host = "192.168.1.20"; saved.name = "living room"; saved.port = 4444;
    CHECK(wiiu::saveSettings(folder, saved, notice));
    saved.name = "new-name";
    CHECK(wiiu::saveSettings(folder, saved, notice));
    CHECK(wiiu::loadSettings(folder, notice).name == "new-name");
    std::filesystem::remove(folder + "/settings.cfg");
    CHECK(wiiu::loadSettings(folder, notice).name == "living_room");
    CHECK(notice == "Recovered settings from SD backup.");
    CHECK(!wiiu::saveSettings("", saved, notice));
    std::filesystem::remove_all(root);
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
}

void transcriptWrapsLongWordsAndSanitizesOnlyDisplay() {
    const std::string utf8 = "caf\xc3\xa9";
    CHECK(wiiu::displayText(utf8) == "caf?");
    std::vector<chat::ChatPayload> messages{{"u", 1, std::string(110, 'x'), "20"}};
    const auto lines = wiiu::chatLines(messages, 51);
    CHECK(lines.size() == 4);
    for (const auto& line : lines) CHECK(line.text.size() <= 51 && line.color == "20");
    CHECK(wiiu::chatLines(messages, 0).empty());
}
}

int main() {
    aromaExitRequestsMenuOnceWithoutStoppingProcuiEarly();
    legacyLauncherExitKeepsLibwhbRelaunchBehavior();
    pcmUsesLittleEndianOnEveryHost();
    microphonePairsAcrossChunksAndDropsMutedPartialFrame();
    voiceGatesCaptureAndReconnectLeavesVoiceOff();
    liveMessagesKeepArrivalOrderAndReconnectHistoryDoesNotReorderThem();
    settingsRejectInvalidInputAndRecoverBackup();
    keyboardCoversPrintableAsciiAndPreservesDraftCursor();
    transcriptWrapsLongWordsAndSanitizesOnlyDisplay();
    std::puts("wiiu_client_test: ok");
}
