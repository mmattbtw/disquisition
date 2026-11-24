#include "audio.h"
#include "lifecycle.h"
#include "render.h"
#include "settings.h"
#include "worker.h"

#include <coreinit/cache.h>
#include <coreinit/screen.h>
#include <coreinit/thread.h>
#include <coreinit/title.h>
#include <coreinit/time.h>
#include <proc_ui/procui.h>
#include <sysapp/launch.h>
#include <vpad/input.h>
#include <whb/log.h>
#include <whb/log_cafe.h>
#include <whb/proc.h>
#include <whb/sdcard.h>

#include <algorithm>
#include <cstdlib>
#include <malloc.h>

namespace {
class ScreenCanvas final : public wiiu::Canvas {
public:
    ScreenCanvas(OSScreenID screen, int width, int height) : Canvas(width, height), screen_(screen) {}
private:
    OSScreenID screen_;
    void putPixel(int x, int y, std::uint32_t color) override {
        OSScreenPutPixelEx(screen_, x, y, color);
    }
};

class Screens {
public:
    ScreenCanvas tv{SCREEN_TV, 1280, 720}, gamepad{SCREEN_DRC, 854, 480};
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
        OSScreenClearBufferEx(SCREEN_TV, wiiu::background);
        OSScreenClearBufferEx(SCREEN_DRC, wiiu::background);
    }
    void end() {
        DCFlushRange(tv_, tvSize_); DCFlushRange(drc_, drcSize_);
        OSScreenFlipBuffersEx(SCREEN_TV); OSScreenFlipBuffersEx(SCREEN_DRC);
    }
private:
    void* tv_ = nullptr;
    void* drc_ = nullptr;
    unsigned tvSize_ = 0, drcSize_ = 0;
};

using wiiu::Pane;
using wiiu::Tab;

class App {
public:
    explicit App(std::string folder) : directory(std::move(folder)), worker(directory) {}
    std::string directory, notice;
    wiiu::Worker worker;
    wiiu::Audio audio;
    Screens screens;
    wiiu::Settings settings;
    bool foreground = true;

    void release() {
        // Voice does not continue in the background. The community stays
        // connected, and the worker reconnects it if the network drops.
        foreground = false;
        stopVoice();
        audio.shutdown();
        screens.release();
    }
    void acquire() {
        screens.acquire();
        audio.initialize(notice);
        foreground = true;
        touchHeld_ = false; held_ = false;
    }

    void connect() {
        if (view_.started) {
            stopVoice(); worker.disconnect(); notice.clear();
            return;
        }
        if (directory.empty()) { notice = "Insert an SD card to connect."; return; }
        worker.connect(settings);
        notice.clear(); scroll_ = 0;
    }

    void frame() {
        // Once exit is requested, only the outer ProcUI loop keeps running.
        if (exit_.requested()) return;
        const auto now = wiiu::nowMilliseconds();
        if (worker.snapshot(view_)) follow();
        // The worker can be busy for a while with TLS handshakes.
        if (voiceWanted_) {
            if (!view_.voiceChannel.empty()) voiceJoined_ = true;
            else if (voiceJoined_ || now - voiceRequestAt_ > 5000) {
                if (!voiceJoined_) notice = "Voice did not join. Try again.";
                stopVoice();
            }
        }
        if (!leaving_.empty() && now - leaveAt_ > 3000) leaving_.clear();
        readInput(now);
        if (exit_.requested()) return;
        const bool gated = editor_.active || !inputAvailable_;
        sendControls(gated);
        const bool transmit = voiceWanted_ && !view_.voiceChannel.empty() && !muted_ && !deafened_ &&
            !gated && (!settings.pushToTalk || held_);
        if (!audio.tick(worker, transmit, deafened_, now, notice)) stopVoice();
        draw();
    }

private:
    enum Edit { Message, Name, Relay, Invite } edit_ = Message;
    console::View view_;
    wiiu::Editor editor_;
    wiiu::ExitRequest exit_;
    Tab tab_ = Tab::Communities;
    Pane pane_ = Pane::Communities;
    std::string draft_, community_, channel_, autoSelected_, leaving_;
    bool muted_ = false, deafened_ = false, held_ = false;
    bool voiceWanted_ = false, voiceJoined_ = false;
    bool touchHeld_ = false, inputAvailable_ = false;
    unsigned controls_ = 0, repeatButton_ = 0;
    std::uint64_t lastInput_ = 0, repeatAt_ = 0, sentRevision_ = 0, voiceRequestAt_ = 0, leaveAt_ = 0;
    std::size_t communityCursor_ = 0, channelCursor_ = 0, scroll_ = 0, memberScroll_ = 0;
    int setting_ = 0;

    const console::CommunityView* community() const { return wiiu::selectedCommunity(view_); }
    std::size_t channelCount() const { return community() ? community()->channels.size() : 0; }
    bool voiceActive() const { return voiceWanted_ || !view_.voiceChannel.empty(); }

    // Keeps list cursors on the session's selection as a new view arrives.
    void follow() {
        if (view_.sentRevision != sentRevision_) { scroll_ = 0; sentRevision_ = view_.sentRevision; }
        if (view_.community != community_) {
            community_ = view_.community;
            for (std::size_t i = 0; i < view_.communities.size(); ++i)
                if (view_.communities[i].id == community_) communityCursor_ = i;
            channelCursor_ = 0;
            if (const auto* selected = community())
                for (std::size_t i = 0; i < selected->channels.size(); ++i)
                    if (selected->channels[i].id == view_.channel) channelCursor_ = i;
            memberScroll_ = 0;
        }
        if (view_.channel != channel_) { channel_ = view_.channel; scroll_ = 0; }
        // Open the first community when none is selected.
        if (view_.started && view_.community.empty()) {
            for (const auto& entry : view_.communities) {
                if (entry.pending) continue;
                if (autoSelected_ != entry.id) { worker.select(entry.id, {}); autoSelected_ = entry.id; }
                break;
            }
        } else autoSelected_.clear();
        const auto communities = view_.communities.size();
        if (communityCursor_ >= communities) communityCursor_ = communities ? communities - 1 : 0;
        if (channelCursor_ >= channelCount()) channelCursor_ = channelCount() ? channelCount() - 1 : 0;
    }

    void sendControls(bool gated) {
        // While editing or without GamePad input, hold back capture locally as
        // unheld push-to-talk rather than announcing a mute to the community.
        const bool pushToTalk = settings.pushToTalk || gated, held = held_ && !gated;
        const unsigned state = 16u | unsigned(muted_) | unsigned(deafened_) << 1 |
            unsigned(pushToTalk) << 2 | unsigned(held) << 3;
        if (state == controls_) return;
        controls_ = state;
        worker.controls(muted_, deafened_, pushToTalk, held);
    }

    void stopVoice() {
        audio.stop();
        if (voiceActive()) worker.leaveVoice();
        voiceWanted_ = voiceJoined_ = false;
    }
    void startVoice(const std::string& id, const std::string& channel) {
        if (!view_.started) { notice = "Connect before joining voice."; return; }
        if (!audio.running() && !audio.start(notice)) return;
        if (!view_.voiceCommunity.empty() && view_.voiceCommunity != id) worker.leaveVoice();
        worker.joinVoice(id, channel);
        controls_ = 0; // joining resets push-to-talk; resend a key already held
        if (!voiceWanted_) voiceJoined_ = false;
        voiceWanted_ = true;
        voiceRequestAt_ = wiiu::nowMilliseconds();
        notice = settings.pushToTalk ? "Voice joined. Hold ZR to talk." : "Voice joined.";
    }
    void toggleVoice() {
        if (voiceActive()) { stopVoice(); return; }
        const auto* selected = community();
        if (!selected) { notice = view_.started ? "Choose a community first." : "Connect before joining voice."; return; }
        // The voice channel under the cursor, otherwise the first one.
        std::string channel;
        if (tab_ == Tab::Communities && pane_ == Pane::Channels && channelCursor_ < channelCount() &&
            selected->channels[channelCursor_].type == "voice")
            channel = selected->channels[channelCursor_].id;
        for (const auto& entry : selected->channels)
            if (channel.empty() && entry.type == "voice") channel = entry.id;
        if (channel.empty()) { notice = "This community has no voice channel."; return; }
        startVoice(selected->id, channel);
    }

    void selectCommunity(std::size_t index) {
        if (index >= view_.communities.size()) return;
        communityCursor_ = index;
        leaving_.clear();
        const auto& entry = view_.communities[index];
        if (!entry.pending && entry.id != view_.community) worker.select(entry.id, {});
    }
    void moveCommunity(int delta) {
        const auto count = view_.communities.size();
        if (count == 0) return;
        if (delta < 0 && communityCursor_ > 0) selectCommunity(communityCursor_ - 1);
        if (delta > 0 && communityCursor_ + 1 < count) selectCommunity(communityCursor_ + 1);
    }
    void openCommunity() {
        if (communityCursor_ >= view_.communities.size()) return;
        if (view_.communities[communityCursor_].pending) { notice = "Still joining this community."; return; }
        selectCommunity(communityCursor_);
        pane_ = Pane::Channels;
    }
    void openChannel() {
        const auto* selected = community();
        if (!selected || channelCursor_ >= selected->channels.size()) return;
        const auto& channel = selected->channels[channelCursor_];
        if (channel.type == "voice") {
            if (view_.voiceCommunity == selected->id && view_.voiceChannel == channel.id) stopVoice();
            else startVoice(selected->id, channel.id);
        } else {
            worker.select(selected->id, channel.id);
            tab_ = Tab::Chat; scroll_ = 0;
        }
    }
    void joinTyped() {
        if (!view_.started) { notice = "Connect before joining a community."; return; }
        edit_ = Invite;
        editor_.open("Invite link", "", 1024);
    }
    void joinFile() {
        if (!view_.started) { notice = "Connect before joining a community."; return; }
        std::string link;
        if (wiiu::readInvite(directory.empty() ? std::string() : directory + "/invite.txt", link, notice)) {
            worker.join(link);
            notice.clear();
        }
    }
    void leave(std::uint64_t now) {
        if (communityCursor_ >= view_.communities.size()) { notice = "Choose a community first."; return; }
        const auto& entry = view_.communities[communityCursor_];
        if (leaving_ != entry.id) {
            leaving_ = entry.id; leaveAt_ = now;
            notice = entry.pending ? "Press Leave again to cancel this invite." :
                "Press Leave again to leave " + entry.name + ".";
            return;
        }
        if (view_.voiceCommunity == entry.id) stopVoice();
        worker.leave(entry.id);
        leaving_.clear(); notice.clear();
    }
    void compose() {
        if (!view_.started) { connect(); return; }
        const auto* selected = community();
        if (!selected || view_.channel.empty()) {
            tab_ = Tab::Communities; notice = "Choose a text channel."; return;
        }
        edit_ = Message;
        editor_.open("Message #" + wiiu::channelName(*selected, view_.channel), draft_, 2000);
    }
    void save() { wiiu::saveSettings(directory, settings, notice); }
    void settingAction(int row) {
        if (row == 4) { connect(); return; }
        if (row == 3) { settings.pushToTalk = !settings.pushToTalk; save(); return; }
        if (view_.started) { notice = "Disconnect to change this setting."; return; }
        if (row == 0) { edit_ = Name; editor_.open("Display name", settings.name, 64); }
        else if (row == 1) { edit_ = Relay; editor_.open("Relay host, optionally host:port", settings.relay, 260); }
        else {
            std::size_t index = 0;
            while (index < wiiu::kColorCount && settings.color != wiiu::kColors[index]) ++index;
            settings.color = wiiu::kColors[(index + 1) % wiiu::kColorCount];
            save();
        }
    }
    void acceptEdit() {
        const auto value = wiiu::trim(editor_.text);
        switch (edit_) {
        case Message:
            draft_ = editor_.text;
            if (value.empty()) { notice = "Enter a message before sending."; return; }
            if (!view_.started) { notice = "Disconnected. Your draft is kept."; break; }
            worker.chat(draft_); draft_.clear(); scroll_ = 0; notice.clear();
            break;
        case Invite:
            if (value.empty()) { notice = "Enter an invite link."; return; }
            worker.join(value); tab_ = Tab::Communities; notice.clear();
            break;
        case Name:
            if (!wiiu::validName(value)) { notice = "Enter a name of 1 to 64 characters."; return; }
            settings.name = value; save();
            break;
        case Relay:
            if (!wiiu::validRelay(value)) { notice = "Enter a relay host, optionally with :port."; return; }
            settings.relay = value; save();
            break;
        }
        editor_.active = false;
    }
    void touch(int x, int y, std::uint64_t now) {
        if (editor_.active) {
            if (editor_.touch(x, y) && editor_.press() == wiiu::Editor::Accepted) acceptEdit();
            return;
        }
        for (int i = 0; i < 4; ++i) if (wiiu::tabRect(i).contains(x, y)) { tab_ = static_cast<Tab>(i); return; }
        if (tab_ == Tab::Settings) {
            for (int i = 0; i < 5; ++i) if (wiiu::settingRect(i).contains(x, y)) {
                setting_ = i; settingAction(i); return;
            }
            return;
        }
        if (wiiu::voiceRect(0).contains(x, y)) toggleVoice();
        else if (wiiu::voiceRect(1).contains(x, y)) muted_ = !muted_;
        else if (wiiu::voiceRect(2).contains(x, y)) deafened_ = !deafened_;
        else if (tab_ != Tab::Communities) {
            if (wiiu::composeRect().contains(x, y)) compose();
        }
        else if (wiiu::actionRect(0).contains(x, y)) joinTyped();
        else if (wiiu::actionRect(1).contains(x, y)) joinFile();
        else if (wiiu::actionRect(2).contains(x, y)) leave(now);
        else {
            const auto communities = view_.communities.size();
            auto first = wiiu::firstVisible(communityCursor_, communities, wiiu::contentRows);
            for (int row = 0; row < wiiu::contentRows; ++row)
                if (wiiu::communityRect(row).contains(x, y) && first + row < communities) {
                    pane_ = Pane::Communities; selectCommunity(first + row); return;
                }
            first = wiiu::firstVisible(channelCursor_, channelCount(), wiiu::contentRows);
            for (int row = 0; row < wiiu::contentRows; ++row)
                if (wiiu::channelRect(row).contains(x, y) && first + row < channelCount()) {
                    pane_ = Pane::Channels; channelCursor_ = first + row; openChannel(); return;
                }
        }
    }
    void readInput(std::uint64_t now) {
        VPADStatus pad{};
        VPADReadError error = VPAD_READ_SUCCESS;
        const int read = VPADRead(VPAD_CHAN_0, &pad, 1, &error);
        if (read <= 0 || error != VPAD_READ_SUCCESS) {
            if (now - lastInput_ > 100) {
                held_ = false; inputAvailable_ = false; touchHeld_ = false;
                if (voiceActive() || audio.running()) {
                    stopVoice();
                    notice = "GamePad unavailable. Voice stopped; chat stays connected.";
                }
            }
            return;
        }
        lastInput_ = now; inputAvailable_ = true; held_ = (pad.hold & VPAD_BUTTON_ZR) != 0;
        unsigned buttons = pad.trigger;
        if (buttons & VPAD_BUTTON_MINUS) {
            stopVoice();
            exit_.request(OSGetTitleID(), SYSLaunchMenu, WHBProcStopRunning);
            return;
        }
        const unsigned directional = pad.hold & (VPAD_BUTTON_UP | VPAD_BUTTON_DOWN | VPAD_BUTTON_LEFT | VPAD_BUTTON_RIGHT);
        if (directional != repeatButton_) { repeatButton_ = directional; repeatAt_ = now + 350; }
        else if (directional && now >= repeatAt_) { buttons |= directional; repeatAt_ = now + 90; }
        VPADTouchData point{};
        VPADGetTPCalibratedPointEx(VPAD_CHAN_0, VPAD_TP_854X480, &point, &pad.tpNormal);
        if (point.touched && point.validity == 0) {
            if (!touchHeld_) touch(point.x, point.y, now);
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
        if (buttons & VPAD_BUTTON_L) tab_ = static_cast<Tab>((int(tab_) + 3) % 4);
        if (buttons & VPAD_BUTTON_R) tab_ = static_cast<Tab>((int(tab_) + 1) % 4);
        if (buttons & VPAD_BUTTON_PLUS) connect();
        if (buttons & VPAD_BUTTON_Y) toggleVoice();
        if (buttons & VPAD_BUTTON_X) muted_ = !muted_;
        if (buttons & VPAD_BUTTON_ZL) deafened_ = !deafened_;
        if (tab_ == Tab::Settings) {
            if (buttons & VPAD_BUTTON_UP) setting_ = (setting_ + 4) % 5;
            if (buttons & VPAD_BUTTON_DOWN) setting_ = (setting_ + 1) % 5;
            if (buttons & VPAD_BUTTON_A) settingAction(setting_);
        } else if (tab_ == Tab::Communities) {
            if (buttons & VPAD_BUTTON_LEFT) pane_ = Pane::Communities;
            if ((buttons & VPAD_BUTTON_RIGHT) && community()) pane_ = Pane::Channels;
            const int delta = (buttons & VPAD_BUTTON_DOWN ? 1 : 0) - (buttons & VPAD_BUTTON_UP ? 1 : 0);
            if (delta && pane_ == Pane::Communities) moveCommunity(delta);
            else if (delta < 0 && channelCursor_ > 0) --channelCursor_;
            else if (delta > 0 && channelCursor_ + 1 < channelCount()) ++channelCursor_;
            if (buttons & VPAD_BUTTON_A) {
                if (pane_ == Pane::Communities) openCommunity();
                else openChannel();
            }
            if (buttons & VPAD_BUTTON_B) { pane_ = Pane::Communities; notice.clear(); }
        } else {
            auto& offset = tab_ == Tab::Chat ? scroll_ : memberScroll_;
            if (buttons & VPAD_BUTTON_UP) ++offset;
            if ((buttons & VPAD_BUTTON_DOWN) && offset > 0) --offset;
            if (buttons & VPAD_BUTTON_B) { scroll_ = memberScroll_ = 0; notice.clear(); }
            if (buttons & VPAD_BUTTON_A) compose();
        }
    }
    void draw() {
        const auto lines = wiiu::chatLines(view_.messages, 51).size();
        scroll_ = std::min(scroll_, lines > 10 ? lines - 10 : 0);
        const auto members = community() ? community()->members.size() : 0;
        memberScroll_ = std::min(memberScroll_, members > 10 ? members - 10 : 0);
        screens.begin();
        wiiu::Scene scene{view_, settings, editor_, notice};
        scene.tab = tab_; scene.pane = pane_;
        scene.community = communityCursor_; scene.channel = channelCursor_;
        scene.scroll = scroll_; scene.memberScroll = memberScroll_; scene.setting = setting_;
        scene.voice = voiceActive(); scene.muted = muted_; scene.deafened = deafened_;
        scene.confirmLeave = !leaving_.empty();
        wiiu::render(scene, screens.tv, screens.gamepad);
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
        App app(sd ? std::string(WHBGetSdCardMountPath()) + "/wiiu/apps/disquisition" : std::string());
        bool saved = false;
        app.settings = wiiu::loadSettings(app.directory, app.notice, saved);
        if (!app.screens.start() || !app.worker.start()) {
            WHBLogPrint("Could not allocate Disquisition screens or network worker.");
            result = 1;
            ProcUIShutdown();
        } else {
            // End the system loading jingle before the first app frame.
            // Microphone capture still waits for an explicit Join voice.
            app.audio.initialize(app.notice);
            ProcUIRegisterCallback(PROCUI_CALLBACK_ACQUIRE, acquired, &app, 100);
            ProcUIRegisterCallback(PROCUI_CALLBACK_RELEASE, released, &app, 100);
            // Like the desktop app, saved settings connect at startup.
            if (saved) app.connect();
            while (WHBProcIsRunning()) {
                if (app.foreground) app.frame();
                OSSleepTicks(OSMillisecondsToTicks(16));
            }
            app.audio.shutdown();
        }
    }
    if (sd) WHBUnmountSdCard();
    WHBLogCafeDeinit();
    WHBProcShutdown();
    return result;
}
