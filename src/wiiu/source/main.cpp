#include "audio.h"
#include "ui.h"
#include "worker.h"

#include <coreinit/cache.h>
#include <coreinit/screen.h>
#include <coreinit/thread.h>
#include <coreinit/time.h>
#include <proc_ui/procui.h>
#include <vpad/input.h>
#include <whb/log.h>
#include <whb/log_cafe.h>
#include <whb/proc.h>
#include <whb/sdcard.h>

#include <algorithm>
#include <cstdlib>
#include <malloc.h>

namespace {
constexpr std::uint32_t background = 0x101C2600;
constexpr std::uint32_t accent = 0xA8DBC700;
constexpr std::uint32_t border = 0x43556300;
constexpr int drcColumns = 53;

class Screens {
public:
    ~Screens() {
        OSScreenShutdown();
        std::free(tv_); std::free(drc_);
    }
    bool start() {
        OSScreenInit();
        tvSize_ = OSScreenGetBufferSizeEx(SCREEN_TV);
        drcSize_ = OSScreenGetBufferSizeEx(SCREEN_DRC);
        tv_ = memalign(256, tvSize_); drc_ = memalign(256, drcSize_);
        if (!tv_ || !drc_) return false;
        acquire(); return true;
    }
    void acquire() {
        OSScreenSetBufferEx(SCREEN_TV, tv_); OSScreenSetBufferEx(SCREEN_DRC, drc_);
        OSScreenEnableEx(SCREEN_TV, true); OSScreenEnableEx(SCREEN_DRC, true);
    }
    void release() {
        OSScreenEnableEx(SCREEN_TV, false); OSScreenEnableEx(SCREEN_DRC, false);
    }
    void begin() {
        OSScreenClearBufferEx(SCREEN_TV, background);
        OSScreenClearBufferEx(SCREEN_DRC, background);
    }
    void end() {
        DCFlushRange(tv_, tvSize_); DCFlushRange(drc_, drcSize_);
        OSScreenFlipBuffersEx(SCREEN_TV); OSScreenFlipBuffersEx(SCREEN_DRC);
    }
    static void text(OSScreenID screen, int column, int row, const std::string& text,
                     std::size_t width = 51) {
        // User-supplied newlines/control bytes never affect the screen layout.
        const auto visible = wiiu::displayText(text).substr(0, width);
        OSScreenPutFontEx(screen, column, row, visible.c_str());
    }
    static void outline(OSScreenID screen, wiiu::Rect r, std::uint32_t color) {
        const int left = r.column * 16, right = (r.column + r.columns) * 16 - 3;
        const int top = r.row * 24, bottom = (r.row + r.rows) * 24 - 3;
        for (int x = left; x <= right; ++x) {
            OSScreenPutPixelEx(screen, x, top, color); OSScreenPutPixelEx(screen, x, bottom, color);
        }
        for (int y = top; y <= bottom; ++y) {
            OSScreenPutPixelEx(screen, left, y, color); OSScreenPutPixelEx(screen, right, y, color);
        }
    }
    static void button(wiiu::Rect r, const std::string& label, bool selected = false) {
        outline(SCREEN_DRC, r, selected ? accent : border);
        text(SCREEN_DRC, r.column + 1, r.row, label, r.columns - 2);
    }
private:
    void* tv_ = nullptr;
    void* drc_ = nullptr;
    unsigned tvSize_ = 0, drcSize_ = 0;
};

class App {
public:
    wiiu::Worker worker;
    wiiu::Audio audio;
    Screens screens;
    bool foreground = true;
    std::string directory, notice;
    wiiu::Settings settings;

    void release() {
        foreground = false;
        audio.stop(); voiceWanted_ = false;
        worker.foreground(false);
        screens.release();
    }
    void acquire() {
        screens.acquire();
        foreground = true;
        worker.foreground(true);
        touchHeld_ = false; held_ = false;
    }

    void frame() {
        const auto now = wiiu::nowMilliseconds();
        view_ = worker.snapshot();
        if (!view_.connected) {
            audio.stop(); voiceWanted_ = false;
        } else if (view_.voice) voiceAcknowledged_ = true;
        else if (voiceAcknowledged_) { audio.stop(); voiceWanted_ = false; voiceAcknowledged_ = false; }
        else if (voiceWanted_ && now - voiceRequestAt_ > 2000) {
            audio.stop(); voiceWanted_ = false; worker.voice(false);
            notice = "Voice did not join. Choose Join voice to retry.";
        }
        if (view_.sentRevision != sentRevision_) { scroll_ = 0; sentRevision_ = view_.sentRevision; }
        readInput(now);
        worker.controls(muted_ || editor_.active || !inputAvailable_, deafened_, settings.pushToTalk, held_);
        auto audioView = view_;
        audioView.deafened = deafened_;
        const bool transmit = view_.connected && voiceWanted_ && !muted_ && !deafened_ &&
            !editor_.active && inputAvailable_ && (!settings.pushToTalk || held_);
        if (!audio.tick(audioView, worker, transmit, now, notice)) {
            voiceWanted_ = false; worker.voice(false);
        }
        draw(now);
    }

private:
    enum Tab { Chat, Members, Settings } tab_ = Chat;
    enum Edit { Message, Host, Port, Name } edit_ = Message;
    handheld::View view_;
    wiiu::Editor editor_;
    std::string draft_;
    bool joining_ = false, muted_ = false, deafened_ = false, held_ = false;
    bool voiceWanted_ = false, voiceAcknowledged_ = false;
    bool touchHeld_ = false, inputAvailable_ = false;
    std::uint64_t lastInput_ = 0, repeatAt_ = 0, sentRevision_ = 0, voiceRequestAt_ = 0;
    unsigned repeatButton_ = 0;
    std::size_t scroll_ = 0, memberScroll_ = 0;
    int setting_ = 0;

    void connect() {
        if (joining_ || view_.connected) {
            worker.leave(); audio.stop(); joining_ = false; voiceWanted_ = false;
            notice = "Disconnected.";
        } else {
            if (!wiiu::validHost(settings.host) || chat::sanitizeName(settings.name).empty()) {
                tab_ = Settings; notice = "Enter a relay host and name first."; return;
            }
            worker.join(settings); joining_ = true;
            muted_ = deafened_ = false; scroll_ = 0;
            notice.clear();
        }
    }
    void voice() {
        if (voiceWanted_ || view_.voice) {
            audio.stop(); worker.voice(false); voiceWanted_ = false; voiceAcknowledged_ = false;
        } else if (view_.connected) {
            if (audio.start(notice)) {
                voiceWanted_ = true; voiceAcknowledged_ = false;
                voiceRequestAt_ = wiiu::nowMilliseconds();
                worker.voice(true); notice = "Voice joined. Hold ZR to talk.";
            }
        } else notice = "Join the relay before joining voice.";
    }
    void beginEdit(Edit kind) {
        if (kind != Message && joining_) { notice = "Disconnect before changing relay settings."; return; }
        edit_ = kind;
        if (kind == Message) {
            if (!view_.connected) { connect(); return; }
            editor_.open("Write a message", draft_, chat::kMaxBodyLength);
        } else if (kind == Host) editor_.open("Relay hostname or IPv4 address", settings.host, 253);
        else if (kind == Port) editor_.open("Relay TCP port", std::to_string(settings.port), 5);
        else editor_.open("Your requested name", settings.name, chat::kMaxNameLength);
    }
    void acceptEdit() {
        if (edit_ == Message) {
            draft_ = chat::sanitizeBody(editor_.text);
            if (!view_.connected) { notice = "Disconnected. Your draft is kept."; editor_.active = false; return; }
            if (draft_.empty()) { notice = "Enter a message before sending."; return; }
            worker.chat(draft_); draft_.clear(); scroll_ = 0;
        } else if (edit_ == Host) {
            const auto value = chat::trim(editor_.text);
            if (!wiiu::validHost(value)) { notice = "Enter a DNS name or IPv4 address, without a port."; return; }
            settings.host = value;
        } else if (edit_ == Port) {
            std::uint16_t port;
            if (!chat::parsePort(editor_.text, port, false)) { notice = "Port must be between 1 and 65535."; return; }
            settings.port = port;
        } else {
            const auto name = chat::sanitizeName(editor_.text);
            if (name.empty()) { notice = "Name cannot be blank."; return; }
            settings.name = name;
        }
        editor_.active = false;
        if (edit_ != Message) wiiu::saveSettings(directory, settings, notice);
    }
    void settingAction(int row) {
        if (row < 3) { beginEdit(row == 0 ? Host : row == 1 ? Port : Name); return; }
        if (row == 3) {
            if (joining_) { notice = "Disconnect before changing color."; return; }
            std::size_t index = 0;
            for (; index < chat::kColorCount; ++index) if (settings.color == chat::kColorNames[index]) break;
            settings.color = chat::kColorNames[(index + 1) % chat::kColorCount];
        } else if (row == 4) settings.pushToTalk = !settings.pushToTalk;
        wiiu::saveSettings(directory, settings, notice);
        if (row == 5) connect();
    }
    void touch(int x, int y) {
        if (editor_.active) {
            if (editor_.touch(x, y) && editor_.press() == wiiu::Editor::Accepted) acceptEdit();
            return;
        }
        for (int i = 0; i < 3; ++i) if (wiiu::Rect{1 + i * 17, 2, 17, 2}.contains(x, y)) {
            tab_ = static_cast<Tab>(i); return;
        }
        if (tab_ == Settings) {
            for (int i = 0; i < 6; ++i) if (wiiu::Rect{1, 4 + i * 2, 51, 2}.contains(x, y)) {
                setting_ = i; settingAction(i); return;
            }
        } else if (wiiu::Rect{1, 14, 51, 2}.contains(x, y)) beginEdit(Message);
        else if (wiiu::Rect{1, 16, 17, 2}.contains(x, y)) voice();
        else if (wiiu::Rect{18, 16, 17, 2}.contains(x, y)) muted_ = !muted_;
        else if (wiiu::Rect{35, 16, 17, 2}.contains(x, y)) deafened_ = !deafened_;
    }
    void readInput(std::uint64_t now) {
        VPADStatus pad{};
        VPADReadError error = VPAD_READ_SUCCESS;
        const int read = VPADRead(VPAD_CHAN_0, &pad, 1, &error);
        if (read <= 0 || error != VPAD_READ_SUCCESS) {
            if (now - lastInput_ > 100) {
                held_ = false; inputAvailable_ = false; touchHeld_ = false;
                if (voiceWanted_ || audio.running()) {
                    audio.stop(); worker.voice(false);
                    voiceWanted_ = voiceAcknowledged_ = false;
                    notice = "GamePad unavailable. Voice stopped; chat stays connected.";
                }
            }
            return;
        }
        lastInput_ = now; inputAvailable_ = true; held_ = (pad.hold & VPAD_BUTTON_ZR) != 0;
        unsigned buttons = pad.trigger;
        const unsigned directional = pad.hold & (VPAD_BUTTON_UP | VPAD_BUTTON_DOWN | VPAD_BUTTON_LEFT | VPAD_BUTTON_RIGHT);
        if (directional != repeatButton_) { repeatButton_ = directional; repeatAt_ = now + 350; }
        else if (directional && now >= repeatAt_) { buttons |= directional; repeatAt_ = now + 90; }
        VPADTouchData point{};
        VPADGetTPCalibratedPointEx(VPAD_CHAN_0, VPAD_TP_854X480, &point, &pad.tpNormal);
        if (point.touched && point.validity == 0) {
            if (!touchHeld_) touch(point.x, point.y);
            touchHeld_ = true;
        } else touchHeld_ = false;
        if (editor_.active) {
            if (buttons & VPAD_BUTTON_B) {
                if (edit_ == Message) draft_ = editor_.text;
                editor_.active = false;
            }
            if (buttons & VPAD_BUTTON_UP) editor_.move(0, -1);
            if (buttons & VPAD_BUTTON_DOWN) editor_.move(0, 1);
            if (buttons & VPAD_BUTTON_LEFT) editor_.move(-1, 0);
            if (buttons & VPAD_BUTTON_RIGHT) editor_.move(1, 0);
            if (buttons & VPAD_BUTTON_X) editor_.backspace();
            if (buttons & VPAD_BUTTON_L) editor_.moveCursor(-1);
            if (buttons & VPAD_BUTTON_R) editor_.moveCursor(1);
            if (buttons & VPAD_BUTTON_Y) editor_.shifted = !editor_.shifted;
            if (buttons & VPAD_BUTTON_PLUS) acceptEdit();
            else if ((buttons & VPAD_BUTTON_A) && editor_.press() == wiiu::Editor::Accepted) acceptEdit();
            return;
        }
        if (buttons & VPAD_BUTTON_MINUS) {
            audio.stop(); worker.leave(); joining_ = false; voiceWanted_ = false; WHBProcStopRunning(); return;
        }
        if (buttons & VPAD_BUTTON_L) tab_ = static_cast<Tab>((int(tab_) + 2) % 3);
        if (buttons & VPAD_BUTTON_R) tab_ = static_cast<Tab>((int(tab_) + 1) % 3);
        if (buttons & VPAD_BUTTON_PLUS) connect();
        if (buttons & VPAD_BUTTON_Y) voice();
        if (buttons & VPAD_BUTTON_X) muted_ = !muted_;
        if (buttons & VPAD_BUTTON_ZL) deafened_ = !deafened_;
        if (tab_ == Settings) {
            if (buttons & VPAD_BUTTON_UP) setting_ = (setting_ + 5) % 6;
            if (buttons & VPAD_BUTTON_DOWN) setting_ = (setting_ + 1) % 6;
            if (buttons & VPAD_BUTTON_A) settingAction(setting_);
        } else {
            auto& offset = tab_ == Chat ? scroll_ : memberScroll_;
            if (buttons & VPAD_BUTTON_UP) ++offset;
            if ((buttons & VPAD_BUTTON_DOWN) && offset > 0) --offset;
            if (buttons & VPAD_BUTTON_B) { scroll_ = memberScroll_ = 0; notice.clear(); }
            if (buttons & VPAD_BUTTON_A) beginEdit(Message);
        }
    }
    void transcript(OSScreenID screen, int columns, int first, int rows, std::size_t offset) {
        const auto lines = wiiu::chatLines(view_.messages, columns - 2);
        const auto maximum = lines.size() > std::size_t(rows) ? lines.size() - rows : 0;
        offset = std::min(offset, maximum);
        const auto end = lines.size() - offset;
        const auto start = end > std::size_t(rows) ? end - rows : 0;
        for (std::size_t i = start; i < end; ++i) {
            const int row = first + int(i - start);
            Screens::text(screen, 1, row, lines[i].text, columns - 2);
            for (int y = row * 24 + 4; y < row * 24 + 20; ++y)
                OSScreenPutPixelEx(screen, 5, y, wiiu::messageColor(lines[i].color));
        }
        if (lines.empty()) Screens::text(screen, 1, first, "Messages appear here after joining.");
    }
    void drawEditor() {
        Screens::text(SCREEN_DRC, 1, 2, editor_.title);
        Screens::text(SCREEN_DRC, 1, 3, notice);
        std::string preview = editor_.text;
        preview.insert(editor_.cursor(), "|");
        const std::size_t start = editor_.cursor() > 180 ? editor_.cursor() - 180 : 0;
        for (int row = 0; row < 4; ++row) {
            const auto index = start + row * 50;
            if (index < preview.size()) Screens::text(SCREEN_DRC, 1, 4 + row, preview.substr(index, 50));
        }
        for (int i = 0; i < 52; ++i)
            Screens::button(editor_.keyRect(i), editor_.key(i), editor_.selected == i);
        Screens::text(SCREEN_DRC, 1, 19, "A key  X delete  B back  + done  L/R cursor");
    }
    void draw(std::uint64_t now) {
        screens.begin();
        Screens::text(SCREEN_TV, 1, 0, "DISQUISITION / WII U", 78);
        Screens::text(SCREEN_TV, 1, 1, view_.status, 78);
        transcript(SCREEN_TV, 80, 3, 23, scroll_);
        Screens::text(SCREEN_TV, 1, 27, "Use the GamePad to write, view members, and change settings.", 78);
        Screens::text(SCREEN_TV, 1, 28, notice, 78);
        Screens::text(SCREEN_DRC, 1, 0, "DISQUISITION / WII U");
        Screens::text(SCREEN_DRC, 1, 1, view_.status);
        if (editor_.active) drawEditor();
        else {
            const char* tabs[] = {"Chat", "Members", "Settings"};
            for (int i = 0; i < 3; ++i) Screens::button({1 + i * 17, 2, 17, 2}, tabs[i], int(tab_) == i);
            if (tab_ == Settings) {
                const std::string fields[] = {"Relay: " + settings.host, "Port: " + std::to_string(settings.port),
                    "Name: " + settings.name, "Color: " + settings.color,
                    settings.pushToTalk ? "Microphone: hold ZR to talk" : "Microphone: open mic",
                    joining_ ? "Save & disconnect" : "Save & join relay"};
                for (int i = 0; i < 6; ++i) Screens::button({1, 4 + i * 2, 51, 2}, fields[i], setting_ == i);
                Screens::text(SCREEN_DRC, 1, 16, "Use a relay port, usually 3333.");
                Screens::text(SCREEN_DRC, 1, 17, "Text settings change while disconnected.");
            } else {
                if (tab_ == Chat) {
                    const auto lines = wiiu::chatLines(view_.messages, drcColumns - 2);
                    scroll_ = std::min(scroll_, lines.size() > 10 ? lines.size() - 10 : 0);
                    transcript(SCREEN_DRC, drcColumns, 4, 10, scroll_);
                } else {
                    memberScroll_ = std::min(memberScroll_, view_.members.size() > 10 ? view_.members.size() - 10 : 0);
                    std::size_t index = 0;
                    for (const auto& [name, member] : view_.members) {
                        if (index++ < memberScroll_) continue;
                        const int row = 4 + int(index - memberScroll_ - 1);
                        if (row >= 14) break;
                        const std::string state = member.deafened ? "deafened" : member.muted ? "muted" :
                            member.lastAudio && now - member.lastAudio < 600 ? "speaking" : member.voice ? "voice" : "text";
                        Screens::text(SCREEN_DRC, 1, row, name + "  " + state);
                    }
                    if (view_.members.empty()) Screens::text(SCREEN_DRC, 1, 4, "Join to see the room's members.");
                }
                Screens::button({1, 14, 51, 2}, view_.connected ? "A / tap to write a message" : joining_ ? "Connecting... + cancels" : "A / tap to join relay");
                Screens::button({1, 16, 17, 2}, voiceWanted_ ? "Leave voice" : "Join voice", voiceWanted_);
                Screens::button({18, 16, 17, 2}, muted_ ? "Unmute" : "Mute", muted_);
                Screens::button({35, 16, 17, 2}, deafened_ ? "Undeafen" : "Deafen", deafened_);
            }
            Screens::text(SCREEN_DRC, 1, 18, notice);
            Screens::text(SCREEN_DRC, 1, 19, "L/R tabs  + join/leave  ZR talk  - exit");
        }
        screens.end();
    }
};

std::uint32_t acquired(void* context) { static_cast<App*>(context)->acquire(); return 0; }
std::uint32_t released(void* context) { static_cast<App*>(context)->release(); return 0; }
}

int main() {
    WHBLogCafeInit();
    WHBProcInit();
    const bool sd = WHBMountSdCard();
    int result = 0;
    {
        App app;
        if (sd) app.directory = std::string(WHBGetSdCardMountPath()) + "/wiiu/apps/disquisition";
        app.settings = wiiu::loadSettings(app.directory, app.notice);
        if (!app.screens.start() || !app.worker.start()) {
            WHBLogPrint("Could not allocate Disquisition screens or network worker.");
            result = 1;
            ProcUIShutdown();
        } else {
            ProcUIRegisterCallback(PROCUI_CALLBACK_ACQUIRE, acquired, &app, 100);
            ProcUIRegisterCallback(PROCUI_CALLBACK_RELEASE, released, &app, 100);
            while (WHBProcIsRunning()) {
                if (app.foreground) app.frame();
                OSSleepTicks(OSMillisecondsToTicks(16));
            }
            app.audio.stop();
        }
    }
    if (sd) WHBUnmountSdCard();
    WHBLogCafeDeinit();
    WHBProcShutdown();
    return result;
}
