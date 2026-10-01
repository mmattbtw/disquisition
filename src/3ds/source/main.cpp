#include "worker.h"
#include "settings_store.h"

#include <3ds.h>
#include <citro2d.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <malloc.h>
#include <utility>

namespace {

using handheld::Settings;
using handheld::View;
using handheld::Worker;

const u32 background = C2D_Color32(23, 27, 30, 255);
const u32 panel = C2D_Color32(34, 40, 43, 255);
const u32 edge = C2D_Color32(51, 60, 63, 255);
const u32 ink = C2D_Color32(234, 238, 229, 255);
const u32 subtle = C2D_Color32(159, 175, 169, 255);
const u32 mint = C2D_Color32(164, 227, 189, 255);
C2D_TextBuf textBuffer = nullptr;

// The system font and keyboard both use Unicode. Replace malformed UTF-8 from
// untrusted peer names without splitting a multibyte character at line breaks.
struct Glyph { std::string bytes; u32 code; };
std::vector<Glyph> glyphs(const std::string& value) {
    std::vector<Glyph> result;
    for (std::size_t i = 0; i < value.size();) {
        const auto first = static_cast<unsigned char>(value[i]);
        unsigned length = first < 0x80 ? 1 : first >= 0xC2 && first <= 0xDF ? 2 :
                          first >= 0xE0 && first <= 0xEF ? 3 : first >= 0xF0 && first <= 0xF4 ? 4 : 0;
        u32 code = length == 1 ? first : first & (0x7F >> length);
        bool valid = length != 0 && i + length <= value.size();
        for (unsigned j = 1; valid && j < length; ++j) {
            const auto part = static_cast<unsigned char>(value[i + j]);
            valid = (part & 0xC0) == 0x80;
            code = (code << 6) | (part & 0x3F);
        }
        if (valid && ((length == 2 && code < 0x80) || (length == 3 && code < 0x800) ||
            (length == 4 && code < 0x10000) || code > 0x10FFFF ||
            (code >= 0xD800 && code <= 0xDFFF))) valid = false;
        if (!valid) { result.push_back({"?", '?'}); ++i; }
        else {
            result.push_back(code < 32 || code == 127 ? Glyph{" ", ' '} :
                             Glyph{value.substr(i, length), code});
            i += length;
        }
    }
    return result;
}

float advance(u32 code, float scale) {
    const auto* width = fontGetCharWidthInfo(nullptr, fontGlyphIndexFromCodePoint(nullptr, code));
    return width->charWidth * scale;
}

std::string fit(const std::string& value, float width, float scale) {
    const auto characters = glyphs(value);
    float total = 0;
    for (const auto& glyph : characters) total += advance(glyph.code, scale);
    const bool truncated = total > width;
    if (truncated) width -= 3 * advance('.', scale);
    std::string output;
    float used = 0;
    for (const auto& glyph : characters) {
        used += advance(glyph.code, scale);
        if (used > width) break;
        output += glyph.bytes;
    }
    if (truncated) output += "...";
    return output;
}

std::vector<std::string> wrap(const std::string& value, float width, float scale) {
    const auto characters = glyphs(value);
    std::vector<std::string> result;
    std::size_t start = 0;
    while (start < characters.size()) {
        float used = 0;
        std::size_t end = start, space = start;
        while (end < characters.size()) {
            const auto& glyph = characters[end];
            if (used + advance(glyph.code, scale) > width && end > start) break;
            used += advance(glyph.code, scale);
            if (glyph.code == ' ') space = end;
            ++end;
        }
        if (end < characters.size() && space > start) end = space;
        std::string line;
        for (std::size_t i = start; i < end; ++i) line += characters[i].bytes;
        result.push_back(std::move(line));
        start = end;
        while (start < characters.size() && characters[start].code == ' ') ++start;
    }
    if (result.empty()) result.push_back("");
    return result;
}

void text(const std::string& value, float x, float y, float scale, u32 color = ink,
          float width = 1000) {
    C2D_Text parsed;
    const auto clipped = fit(value, width, scale);
    C2D_TextParse(&parsed, textBuffer, clipped.c_str());
    C2D_TextOptimize(&parsed);
    C2D_DrawText(&parsed, C2D_WithColor, x, y, 0, scale, scale, color);
}
void rect(float x, float y, float w, float h, u32 color) {
    C2D_DrawRectSolid(x, y, 0, w, h, color);
}
void button(const std::string& label, float x, float y, float w, float h,
            bool selected = false, bool enabled = true) {
    rect(x, y, w, h, selected ? mint : panel);
    text(label, x + 10, y + (h - 15) / 2, .48f,
         !enabled ? subtle : selected ? background : ink, w - 12);
}
bool hit(const touchPosition& touch, int x, int y, int w, int h) {
    return touch.px >= x && touch.px < x + w && touch.py >= y && touch.py < y + h;
}

u32 chatColor(const std::string& color) {
    const u32 named[] = {C2D_Color32(255, 175, 215, 255), mint,
        C2D_Color32(255, 255, 175, 255), C2D_Color32(135, 175, 255, 255),
        C2D_Color32(215, 175, 255, 255), C2D_Color32(175, 255, 255, 255),
        C2D_Color32(255, 175, 135, 255)};
    for (std::size_t i = 0; i < chat::kColorCount; ++i)
        if (color == chat::kColorNames[i]) return named[i];
    int index = 0;
    if (!chat::parseColorIndex(color, index)) return mint;
    if (index >= 232) {
        const auto gray = static_cast<u8>(8 + (index - 232) * 10);
        return C2D_Color32(gray, gray, gray, 255);
    }
    if (index >= 16) {
        const u8 levels[] = {0, 95, 135, 175, 215, 255};
        index -= 16;
        return C2D_Color32(levels[index / 36], levels[(index / 6) % 6], levels[index % 6], 255);
    }
    const u8 basic[16][3] = {{0,0,0},{205,49,49},{13,188,121},{229,229,16},
        {36,114,200},{188,63,188},{17,168,205},{229,229,229},
        {102,102,102},{241,76,76},{35,209,139},{245,245,67},
        {59,142,234},{214,112,214},{41,184,219},{255,255,255}};
    return C2D_Color32(basic[index][0], basic[index][1], basic[index][2], 255);
}

struct HookContext { Worker* worker; std::atomic<bool> keyboard{false}; };
void lifecycle(APT_HookType type, void* data) {
    auto& context = *static_cast<HookContext*>(data);
    if (type == APTHOOK_ONSLEEP || (type == APTHOOK_ONSUSPEND && !context.keyboard.load()))
        context.worker->suspend();
}

bool keyboard(Worker& worker, HookContext& context, const View& view,
              const char* hint, std::string& value, int limit, bool number = false) {
    // Keep network and receive audio running on the worker, but never retain
    // a held PTT key or record open-mic audio while inside a system applet.
    worker.controls(true, view.deafened, view.pushToTalk, false);
    const u64 deadline = osGetTime() + 1000;
    while (worker.snapshot().transmitting && osGetTime() < deadline)
        svcSleepThread(5 * 1000 * 1000LL);
    if (worker.snapshot().transmitting) return false;
    SwkbdState state;
    swkbdInit(&state, number ? SWKBD_TYPE_NUMPAD : SWKBD_TYPE_NORMAL, 2, limit);
    swkbdSetValidation(&state, SWKBD_NOTEMPTY_NOTBLANK, 0, 0);
    swkbdSetHintText(&state, hint);
    swkbdSetInitialText(&state, value.c_str());
    swkbdSetButton(&state, SWKBD_BUTTON_LEFT, "Cancel", false);
    swkbdSetButton(&state, SWKBD_BUTTON_RIGHT, "OK", true);
    std::vector<char> output(static_cast<std::size_t>(limit) * 4 + 1);
    context.keyboard = true;
    const auto pressed = swkbdInputText(&state, output.data(), output.size());
    context.keyboard = false;
    hidScanInput();
    worker.controls(view.muted, view.deafened, view.pushToTalk, false);
    if (pressed != SWKBD_BUTTON_RIGHT) return false;
    value = chat::trim(output.data());
    return true;
}

struct Row { std::string value, time; u32 color; bool heading; };
std::vector<Row> chatRows(const View& view, std::map<std::string, std::vector<Row>>& cache) {
    std::vector<Row> rows;
    std::map<std::string, std::vector<Row>> retained;
    for (const auto& message : view.messages) {
        auto key = chat::encode({chat::MsgType::PeerChat,
            {message.sender, std::to_string(message.timestamp), message.body, message.color}});
        std::vector<Row> layout;
        const auto found = cache.find(key);
        if (found != cache.end()) layout = std::move(found->second);
        else {
            char stamp[16] = "";
            const auto seconds = static_cast<time_t>(message.timestamp);
            if (const auto* time = std::localtime(&seconds)) std::strftime(stamp, sizeof(stamp), "%H:%M", time);
            layout.push_back({message.sender, stamp, chatColor(message.color), true});
            for (auto& line : wrap(message.body, 364, .45f))
                layout.push_back({std::move(line), {}, ink, false});
            layout.push_back({"", {}, ink, false});
        }
        rows.insert(rows.end(), layout.begin(), layout.end());
        retained.emplace(std::move(key), std::move(layout));
    }
    cache.swap(retained);
    return rows;
}

void topScreen(C3D_RenderTarget* target, const View& view, const std::vector<Row>& rows,
               int scroll, const std::string& notice) {
    C2D_TargetClear(target, background);
    C2D_SceneBegin(target);
    text("Disquisition", 14, 6, .72f);
    text(view.connected ? view.name : "3DS", 270, 13, .44f, mint, 120);
    text(view.status, 14, 34, .39f, subtle, 372);
    rect(14, 52, 372, 1, edge);
    if (rows.empty()) {
        text(view.connected ? "You're in. Say hello." : "Your chat, on two screens.", 18, 84, .62f, mint, 364);
        text(view.connected ? "A opens the keyboard. Y joins voice." : "Set your relay in Settings, then tap Join.",
             18, 119, .44f, ink, 364);
        text("Messages appear here. Controls are below.", 18, 145, .43f, subtle, 364);
    } else {
        const int end = std::max(0, static_cast<int>(rows.size()) - scroll);
        const int start = std::max(0, end - 10);
        for (int i = start; i < end; ++i) {
            const auto& row = rows[static_cast<std::size_t>(i)];
            const float y = 59 + (i - start) * 16;
            text(row.value, row.heading ? 16 : 22, y, .45f, row.color, row.heading ? 310 : 364);
            if (row.heading) text(row.time, 345, y, .36f, subtle, 45);
        }
    }
    rect(0, 222, 400, 18, panel);
    text(!notice.empty() ? notice : scroll > 0 ? "D-pad scrolls  |  R returns to newest" : "D-pad scrolls  |  START exits",
         14, 224, .36f, subtle, 372);
}

void bottomScreen(C3D_RenderTarget* target, const Settings& settings, const View& view,
                  int tab, int memberScroll, bool joining, bool networkAvailable,
                  bool muted, bool deafened) {
    C2D_TargetClear(target, background);
    C2D_SceneBegin(target);
    const char* tabs[] = {"Chat", "Members", "Settings"};
    for (int i = 0; i < 3; ++i) {
        text(tabs[i], 14 + i * 106, 9, .5f, tab == i ? mint : subtle, 94);
        if (tab == i) rect(i * 106 + 12, 32, 82, 2, mint);
    }
    if (tab == 0) {
        button(view.connected ? "Write a message... (A)" : joining ? "Leave / cancel" : "Join relay (A)",
               12, 47, 296, 43, !view.connected && !joining, networkAvailable);
        text(view.connected ? "Tap to open the 3DS keyboard" : "Uses the relay in Settings",
             21, 78, .31f, view.connected || joining ? subtle : background, 278);
        button(view.voice ? "Leave voice (Y)" : "Join voice (Y)", 12, 102, 296, 35,
               view.voice, view.connected);
        button(muted ? "Unmute (X)" : "Mute (X)", 12, 149, 142, 33, muted, view.voice);
        button(deafened ? "Undeafen" : "Deafen", 166, 149, 142, 33, deafened, view.voice);
        text(view.voice ? deafened ? "Deafened. Microphone is also muted." : muted ? "Microphone muted." :
             settings.pushToTalk ? view.transmitting ? "Talking. Release L to stop." : "Hold L to talk." :
             "Open microphone. Tap Mute to stop." : "Voice starts only when you join it.",
             16, 195, .4f, view.transmitting ? mint : subtle, 292);
    } else if (tab == 1) {
        text(std::to_string(view.members.size()) + " in this chat", 14, 44, .45f, subtle, 292);
        int index = 0;
        const auto now = static_cast<std::uint64_t>(osGetTime());
        for (const auto& [name, member] : view.members) {
            if (index++ < memberScroll) continue;
            const int row = index - 1 - memberScroll;
            if (row >= 5) break;
            const int y = 70 + row * 28;
            rect(12, y, 296, 25, panel);
            const bool talking = member.lastAudio != 0 && now - member.lastAudio < 350;
            text(name + (name == view.name ? " (you)" : ""), 20, y + 4, .43f,
                 talking ? mint : ink, 183);
            text(member.deafened ? "deafened" : member.muted && member.voice ? "muted" :
                 member.voice ? "voice" : "text", 217, y + 5, .37f, talking ? mint : subtle, 84);
        }
        if (view.members.empty()) text("Join a relay to see the members.", 16, 80, .43f, ink, 292);
        text("D-pad scrolls the member list", 14, 212, .34f, subtle, 292);
    } else {
        button("Relay: " + (settings.host.empty() ? "tap to set host" : settings.host), 12, 44, 296, 31);
        button("Port: " + std::to_string(settings.port) + "    Name: " + settings.name, 12, 82, 296, 31);
        button("Color: " + settings.color, 12, 120, 296, 31);
        rect(286, 127, 13, 13, chatColor(settings.color));
        button(settings.pushToTalk ? "Voice: push-to-talk (hold L)" : "Voice: open microphone", 12, 158, 296, 31);
        button(joining ? "Leave relay" : "Save & join relay", 12, 197, 296, 29, !joining, networkAvailable);
    }
    if (tab != 2) {
        rect(0, 226, 320, 14, panel);
        text("B Chat   SELECT Settings", 12, 227, .31f, subtle, 296);
    }
}

} // namespace

int main() {
    gfxInitDefault();
    gfxSet3D(false);
    if (!C3D_Init(C3D_DEFAULT_CMDBUF_SIZE) || !C2D_Init(C2D_DEFAULT_MAX_OBJECTS)) {
        gfxExit();
        return 1;
    }
    C2D_Prepare();
    auto* top = C2D_CreateScreenTarget(GFX_TOP, GFX_LEFT);
    auto* bottom = C2D_CreateScreenTarget(GFX_BOTTOM, GFX_LEFT);
    textBuffer = C2D_TextBufNew(8192);
    if (!top || !bottom || !textBuffer) {
        if (textBuffer) C2D_TextBufDelete(textBuffer);
        C2D_Fini(); C3D_Fini(); gfxExit();
        return 1;
    }
    u32* socketMemory = static_cast<u32*>(memalign(0x1000, 1024 * 1024));
    const Result networkResult = socketMemory ? socInit(socketMemory, 1024 * 1024) :
        static_cast<Result>(MAKERESULT(RL_PERMANENT, RS_OUTOFRESOURCE, RM_APPLICATION, RD_OUT_OF_MEMORY));
    const bool networkAvailable = R_SUCCEEDED(networkResult);
    std::string storageError;
    Settings settings = handheld::loadSettings(storageError);
    {
        Worker worker;
        const bool workerAvailable = networkAvailable && worker.start();
        HookContext context{&worker};
        aptHookCookie cookie;
        aptHook(&cookie, lifecycle, &context);
        int tab = settings.host.empty() ? 2 : 0;
        int scroll = 0, memberScroll = 0;
        bool joining = false, muted = false, deafened = false;
        std::string notice = storageError;
        if (!networkAvailable) {
            char code[16];
            std::snprintf(code, sizeof(code), "0x%08lX", static_cast<unsigned long>(static_cast<u32>(networkResult)));
            notice = std::string("Socket service: ") + code;
        } else if (!workerAvailable) notice = "Could not start network worker.";
        std::uint64_t revision = ~std::uint64_t(0);
        std::uint64_t sentRevision = 0;
        std::vector<Row> rows;
        std::map<std::string, std::vector<Row>> rowCache;
        auto join = [&] {
            if (!workerAvailable) return;
            if (joining) { worker.leave(); joining = false; return; }
            if (settings.host.empty()) { tab = 2; notice = "Tap Relay to enter its host or IPv4 address."; return; }
            if (!handheld::saveSettings(settings, storageError)) notice = storageError + "; settings not saved";
            else notice.clear();
            worker.join(settings);
            joining = true;
            muted = deafened = false;
            tab = 0;
        };
        while (aptMainLoop()) {
            hidScanInput();
            const u32 down = hidKeysDown();
            if (down & KEY_START) break;
            View view = worker.snapshot();
            if (view.sentRevision != sentRevision) {
                scroll = 0;
                sentRevision = view.sentRevision;
            }
            if (view.revision != revision) {
                const int oldSize = static_cast<int>(rows.size());
                rows = chatRows(view, rowCache);
                if (scroll > 0) scroll += std::max(0, static_cast<int>(rows.size()) - oldSize);
                revision = view.revision;
            }
            if (down & KEY_SELECT) tab = 2;
            if (down & KEY_B) { tab = 0; notice.clear(); }
            if (down & KEY_UP) { if (tab == 1) ++memberScroll; else scroll += 3; }
            if (down & KEY_DOWN) { if (tab == 1) --memberScroll; else scroll -= 3; }
            if (down & KEY_R) scroll = 0;
            scroll = std::clamp(scroll, 0, std::max(0, static_cast<int>(rows.size()) - 10));
            memberScroll = std::clamp(memberScroll, 0, std::max(0, static_cast<int>(view.members.size()) - 5));
            bool compose = (down & KEY_A) && tab == 0 && view.connected;
            bool toggleVoice = down & KEY_Y;
            if ((down & KEY_A) && tab == 0 && !view.connected) join();
            if (down & KEY_X) muted = !muted;
            if (down & KEY_TOUCH) {
                touchPosition touch;
                hidTouchRead(&touch);
                if (touch.py < 36) tab = std::min(2, static_cast<int>(touch.px / 106));
                else if (tab == 0) {
                    if (hit(touch, 12, 47, 296, 43)) {
                        if (view.connected) compose = true; else join();
                    } else if (hit(touch, 12, 102, 296, 35)) toggleVoice = true;
                    else if (hit(touch, 12, 149, 142, 33)) muted = !muted;
                    else if (hit(touch, 166, 149, 142, 33)) deafened = !deafened;
                } else if (tab == 2) {
                    if (hit(touch, 12, 197, 296, 29)) join();
                    else if (hit(touch, 12, 158, 296, 31)) {
                        settings.pushToTalk = !settings.pushToTalk;
                        if (!handheld::saveSettings(settings, storageError)) notice = storageError + "; settings not saved";
                        else notice.clear();
                    } else if (joining) notice = "Leave the relay before changing connection settings.";
                    else if (hit(touch, 12, 44, 296, 31)) {
                        std::string value = settings.host;
                        if (keyboard(worker, context, view, "Relay host or IPv4 address", value, 253)) {
                            if (value.find_first_of(" \t\r\n/=:") != std::string::npos || value.size() > 253)
                                notice = "Enter a host only. Set its port in the next row.";
                            else { settings.host = value; notice.clear(); }
                        }
                    } else if (hit(touch, 12, 82, 120, 31)) {
                        std::string value = std::to_string(settings.port);
                        if (keyboard(worker, context, view, "Relay port, 1 to 65535", value, 5, true) &&
                            !chat::parsePort(value, settings.port, false)) notice = "Port must be 1 to 65535.";
                    } else if (hit(touch, 132, 82, 176, 31)) {
                        std::string value = settings.name;
                        if (keyboard(worker, context, view, "Your chat name", value, 20)) {
                            settings.name = chat::sanitizeName(value);
                            if (settings.name.empty()) settings.name = "3ds";
                        }
                    } else if (hit(touch, 12, 120, 296, 31)) {
                        const auto found = std::find(chat::kColorNames.begin(), chat::kColorNames.end(), settings.color);
                        const auto index = found == chat::kColorNames.end() ? 0 :
                            (static_cast<std::size_t>(found - chat::kColorNames.begin()) + 1) % chat::kColorCount;
                        settings.color = chat::kColorNames[index];
                    }
                }
            }
            if (compose) {
                std::string body;
                if (keyboard(worker, context, view, "Message to everyone", body, 500)) {
                    worker.chat(body);
                    scroll = 0;
                    notice.clear();
                }
            }
            if (toggleVoice && view.connected) { worker.voice(!view.voice); notice.clear(); }
            if (workerAvailable) worker.controls(muted, deafened, settings.pushToTalk, hidKeysHeld() & KEY_L);
            C2D_TextBufClear(textBuffer);
            C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
            topScreen(top, view, rows, scroll, notice);
            bottomScreen(bottom, settings, view, tab, memberScroll, joining,
                         workerAvailable, muted, deafened);
            C3D_FrameEnd(0);
        }
        aptUnhook(&cookie);
        if (!settings.host.empty()) handheld::saveSettings(settings, storageError);
    } // Stop the worker and release MIC/NDSP before tearing down sockets/graphics.
    if (networkAvailable) socExit();
    std::free(socketMemory);
    C2D_TextBufDelete(textBuffer);
    C2D_Fini(); C3D_Fini(); gfxExit();
    return 0;
}
