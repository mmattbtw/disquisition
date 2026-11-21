#include "audio.h"
#include "settings.h"
#include "storage.h"
#include "worker.h"

#include <3ds.h>
#include <citro2d.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <exception>
#include <malloc.h>
#include <map>
#include <utility>
#include <vector>

namespace {

using console::ChannelView;
using console::CommunityView;
using console::View;
using handheld::Settings;
using handheld::Worker;

const u32 background = C2D_Color32(23, 27, 30, 255);
const u32 panel = C2D_Color32(34, 40, 43, 255);
const u32 edge = C2D_Color32(51, 60, 63, 255);
const u32 ink = C2D_Color32(234, 238, 229, 255);
const u32 subtle = C2D_Color32(159, 175, 169, 255);
const u32 mint = C2D_Color32(164, 227, 189, 255);
C2D_TextBuf textBuffer = nullptr;

enum Tab { Communities, Channels, Chat, Members, SettingsTab, TabCount };
const char* const kTabs[TabCount] = {"Communities", "Channels", "Chat", "Members", "Settings"};
constexpr int kTabWidth = 64;
constexpr int kListRows = 5;
constexpr int kChatRows = 10;

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
// A list row with a label, an optional detail on the right, and an unread dot.
void row(const std::string& label, const std::string& detail, float y, bool selected,
         bool unread, u32 color = ink) {
    rect(12, y, 296, 24, selected ? mint : panel);
    text(label, 20, y + 4, .43f, selected ? background : color, detail.empty() ? 268 : 160);
    if (!detail.empty()) text(detail, 186, y + 5, .37f, selected ? background : subtle, 104);
    if (unread) rect(296, y + 9, 6, 6, selected ? background : mint);
}
bool hit(const touchPosition& touch, int x, int y, int w, int h) {
    return touch.px >= x && touch.px < x + w && touch.py >= y && touch.py < y + h;
}
float listY(int index, float top) { return top + index * 27; }

u32 xterm(int index) {
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

// Matches the desktop's chat colors: names, #RRGGBB, or xterm indexes.
u32 chatColor(const std::string& color) {
    const int named[] = {211, 121, 229, 111, 183, 159, 216};
    for (std::size_t i = 0; i < handheld::kColorNames.size(); ++i)
        if (color == handheld::kColorNames[i]) return xterm(named[i]);
    if (!handheld::validColor(color)) return xterm(121);
    if (color[0] != '#') return xterm(std::stoi(color));
    const auto value = std::stoul(color.substr(1), nullptr, 16);
    return C2D_Color32(static_cast<u8>(value >> 16), static_cast<u8>(value >> 8),
                       static_cast<u8>(value), 255);
}

const CommunityView* findCommunity(const View& view, const std::string& id) {
    for (const auto& community : view.communities)
        if (community.id == id) return &community;
    return nullptr;
}
const ChannelView* findChannel(const CommunityView* community, const std::string& id) {
    if (community)
        for (const auto& channel : community->channels)
            if (channel.id == id) return &channel;
    return nullptr;
}
const ChannelView* firstVoice(const CommunityView* community) {
    if (community)
        for (const auto& channel : community->channels)
            if (channel.type == "voice") return &channel;
    return nullptr;
}

struct HookContext { Worker* worker; std::atomic<bool> keyboard{false}; };
void lifecycle(APT_HookType type, void* data) {
    auto& context = *static_cast<HookContext*>(data);
    if (type == APTHOOK_ONSLEEP || (type == APTHOOK_ONSUSPEND && !context.keyboard.load())) {
        // The worker can be busy with DNS or TLS for a while. Stop recording
        // before the transition; the worker then leaves voice and frees audio.
        handheld::micGate().suspend();
        context.worker->suspend();
    }
    else if (type == APTHOOK_ONRESTORE || type == APTHOOK_ONWAKEUP)
        handheld::micGate().resume();
}

bool keyboard(Worker& worker, HookContext& context, bool muted, bool deafened, bool pushToTalk,
              const char* hint, std::string& value, int limit, SwkbdType type) {
    // Keep network and receive audio running on the worker, but never retain
    // a held PTT key or send open-mic audio while inside a system applet.
    handheld::micGate().paused = true;
    worker.controls(true, deafened, pushToTalk, false);
    SwkbdState state;
    swkbdInit(&state, type, 2, limit);
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
    worker.controls(muted, deafened, pushToTalk, false);
    handheld::micGate().paused = false;
    if (pressed != SWKBD_BUTTON_RIGHT) return false;
    value = handheld::trim(output.data());
    return true;
}

struct Row { std::string value, time; u32 color; bool heading; };
std::vector<Row> chatRows(const View& view, std::map<std::string, std::vector<Row>>& cache) {
    std::vector<Row> rows;
    std::map<std::string, std::vector<Row>> retained;
    for (const auto& message : view.messages) {
        auto key = message.peer + '\n' + message.sender + '\n' + std::to_string(message.timestamp) +
            '\n' + message.color + (message.undelivered ? "\n!\n" : "\n\n") + message.body;
        std::vector<Row> layout;
        const auto found = cache.find(key);
        if (found != cache.end()) layout = found->second;
        else {
            char stamp[16] = "";
            const auto seconds = static_cast<time_t>(message.timestamp / 1000);
            if (const auto* time = std::localtime(&seconds)) std::strftime(stamp, sizeof(stamp), "%H:%M", time);
            layout.push_back({message.sender, stamp, chatColor(message.color), true});
            for (auto& line : wrap(message.body, 364, .45f))
                layout.push_back({std::move(line), {}, ink, false});
            if (message.undelivered)
                layout.push_back({"Not delivered: nobody else was online.", {}, subtle, false});
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
    const auto* community = findCommunity(view, view.community);
    const auto* channel = findChannel(community, view.channel);
    text(community ? community->name : "Disquisition", 14, 6, .62f, ink, channel ? 150 : 250);
    if (channel) text("#" + channel->name, 170, 11, .5f, subtle, 100);
    if (view.started) {
        rect(276, 15, 7, 7, view.relayConnected ? mint : edge);
        text(view.name, 289, 11, .44f, mint, 97);
    }
    text(view.status, 14, 34, .39f, subtle, 372);
    rect(14, 52, 372, 1, edge);
    if (rows.empty()) {
        text(!view.started ? "Not connected." : !community ? "No community selected." :
             !channel ? "No channel selected." : "No messages yet.", 18, 84, .5f, subtle, 364);
    } else {
        const int end = std::max(0, static_cast<int>(rows.size()) - scroll);
        const int start = std::max(0, end - kChatRows);
        for (int i = start; i < end; ++i) {
            const auto& line = rows[static_cast<std::size_t>(i)];
            const float y = 59 + (i - start) * 16;
            text(line.value, line.heading ? 16 : 22, y, .45f, line.color, line.heading ? 310 : 364);
            if (line.heading) text(line.time, 345, y, .36f, subtle, 45);
        }
    }
    rect(0, 222, 400, 18, panel);
    text(!notice.empty() ? notice : scroll > 0 ? "D-pad scrolls  |  R returns to newest" : "D-pad scrolls  |  START exits",
         14, 224, .36f, subtle, 372);
}

struct Screen {
    int tab = Communities;
    int list[TabCount] = {};
    bool muted = false;
    bool deafened = false;
    std::string chosen;  // last tapped community, which may still be joining
    std::string confirm; // community awaiting a second Leave tap
};

// The community Leave applies to: a tapped pending join, else the selected one.
std::string leaveTarget(const View& view, const Screen& screen) {
    const auto* chosen = findCommunity(view, screen.chosen);
    return chosen && chosen->pending ? chosen->id : view.community;
}

std::string channelName(const CommunityView* community, const std::string& id) {
    const auto* channel = findChannel(community, id);
    return channel ? channel->name : std::string("voice");
}

void bottomScreen(C3D_RenderTarget* target, const Settings& settings, const View& view,
                  const Screen& screen, bool available) {
    C2D_TargetClear(target, background);
    C2D_SceneBegin(target);
    const auto* community = view.started ? findCommunity(view, view.community) : nullptr;
    const char* missing = view.started ? "No community selected." : "Not connected.";
    bool otherUnread = false, channelUnread = false;
    for (const auto& entry : view.communities)
        if (entry.unread && entry.id != view.community) otherUnread = true;
    if (community)
        for (const auto& channel : community->channels)
            if (channel.unread && channel.id != view.channel) channelUnread = true;
    for (int i = 0; i < TabCount; ++i) {
        float width = 0;
        for (const auto& glyph : glyphs(kTabs[i])) width += advance(glyph.code, .42f);
        text(kTabs[i], i * kTabWidth + std::max(2.f, (kTabWidth - width) / 2), 10, .42f,
             screen.tab == i ? mint : subtle, kTabWidth - 4);
        if (screen.tab == i) rect(i * kTabWidth + 4, 32, kTabWidth - 8, 2, mint);
        if ((i == Communities && otherUnread) || (i == Channels && channelUnread))
            rect(i * kTabWidth + kTabWidth - 7, 4, 5, 5, mint);
    }
    const int first = screen.list[screen.tab];
    if (screen.tab == Communities) {
        if (!view.started) {
            button("Connect (A)", 12, 47, 296, 43, true, available);
            text(settings.relay, 21, 78, .31f, background, 278);
        } else {
            if (view.communities.empty()) text("No communities yet.", 16, 52, .43f, subtle, 292);
            for (int i = 0; i < kListRows && first + i < static_cast<int>(view.communities.size()); ++i) {
                const auto& entry = view.communities[static_cast<std::size_t>(first + i)];
                const bool selected = entry.pending ? entry.id == screen.chosen : entry.id == view.community;
                row(entry.name, entry.id == view.voiceCommunity ? "voice" : "", listY(i, 42),
                    selected, entry.unread, entry.pending ? subtle : ink);
            }
            const auto leaving = leaveTarget(view, screen);
            const auto* entry = findCommunity(view, leaving);
            button("Type invite", 12, 182, 96, 30);
            button("invite.txt", 112, 182, 96, 30);
            button(!leaving.empty() && screen.confirm == leaving ? "Confirm" :
                   entry && entry->pending ? "Cancel join" : "Leave",
                   212, 182, 96, 30, false, !leaving.empty());
        }
    } else if (screen.tab == Channels) {
        if (!community) text(missing, 16, 52, .43f, subtle, 292);
        else {
            text(community->name, 14, 42, .45f, mint, 292);
            for (int i = 0; i < kListRows && first + i < static_cast<int>(community->channels.size()); ++i) {
                const auto& channel = community->channels[static_cast<std::size_t>(first + i)];
                if (channel.type == "text") {
                    row("# " + channel.name, "", listY(i, 64), channel.id == view.channel, channel.unread);
                    continue;
                }
                int present = 0;
                for (const auto& member : community->members)
                    if (member.online && member.voiceChannel == channel.id) ++present;
                const bool joined = view.voiceCommunity == community->id && view.voiceChannel == channel.id;
                row(channel.name, joined ? "Leave voice" : present ? "Join voice (" + std::to_string(present) + ")" :
                    "Join voice", listY(i, 64), joined, false);
            }
        }
    } else if (screen.tab == Chat) {
        const auto* channel = findChannel(community, view.channel);
        const auto* voice = firstVoice(community);
        const bool inVoice = !view.voiceChannel.empty();
        button(!view.started ? "Connect (A)" : channel ? "Write a message (A)" :
               community ? "Choose a channel" : "Choose a community", 12, 47, 296, 43,
               !view.started, available);
        text(!view.started ? settings.relay : channel ? "#" + channel->name + " in " + community->name : "",
             21, 78, .31f, !view.started ? background : subtle, 278);
        button(inVoice ? "Leave voice (Y)" : voice ? "Join " + voice->name + " (Y)" : "Join voice (Y)",
               12, 102, 296, 35, inVoice, inVoice || voice);
        button(screen.muted ? "Unmute (X)" : "Mute (X)", 12, 149, 142, 33, screen.muted, inVoice);
        button(screen.deafened ? "Undeafen" : "Deafen", 166, 149, 142, 33, screen.deafened, inVoice);
        if (inVoice) {
            const auto* voiceCommunity = findCommunity(view, view.voiceCommunity);
            text(channelName(voiceCommunity, view.voiceChannel) + ": " +
                 (screen.deafened ? "deafened, microphone muted." : screen.muted ? "microphone muted." :
                  settings.pushToTalk ? view.transmitting ? "talking. Release L to stop." : "hold L to talk." :
                  "open microphone."),
                 16, 195, .4f, view.transmitting ? mint : subtle, 292);
        }
    } else if (screen.tab == Members) {
        if (!community) text(missing, 16, 52, .43f, subtle, 292);
        else {
            int online = 0;
            for (const auto& member : community->members) online += member.online;
            text(std::to_string(online) + " online, " + std::to_string(community->members.size()) +
                 " members", 14, 42, .45f, subtle, 292);
            for (int i = 0; i < kListRows && first + i < static_cast<int>(community->members.size()); ++i) {
                const auto& member = community->members[static_cast<std::size_t>(first + i)];
                std::string detail = !member.online ? "offline" : member.voiceChannel.empty() ? "online" :
                    channelName(community, member.voiceChannel);
                if (member.online && member.deafened) detail += ", deafened";
                else if (member.online && member.muted && !member.voiceChannel.empty()) detail += ", muted";
                row(member.name + (member.self ? " (you)" : ""), detail, listY(i, 64), false, false,
                    member.speaking ? mint : member.online ? ink : subtle);
            }
        }
    } else {
        button("Name: " + settings.name, 12, 44, 296, 31);
        button("Relay: " + settings.relay, 12, 82, 296, 31);
        button("Color: " + settings.color, 12, 120, 296, 31);
        rect(286, 129, 13, 13, chatColor(settings.color));
        button(settings.pushToTalk ? "Voice: push-to-talk (hold L)" : "Voice: open microphone", 12, 158, 296, 31);
        button(view.started ? "Disconnect" : "Connect", 12, 197, 296, 29, !view.started, available);
    }
    if (screen.tab != SettingsTab) {
        rect(0, 226, 320, 14, panel);
        text("B Chat   SELECT Settings", 12, 227, .31f, subtle, 296);
    }
}

int listSize(const View& view, int tab) {
    const auto* community = view.started ? findCommunity(view, view.community) : nullptr;
    if (tab == Communities) return static_cast<int>(view.communities.size());
    if (tab == Channels) return community ? static_cast<int>(community->channels.size()) : 0;
    if (tab == Members) return community ? static_cast<int>(community->members.size()) : 0;
    return 0;
}

std::string resultCode(Result result) {
    char code[16];
    std::snprintf(code, sizeof(code), "0x%08lX", static_cast<unsigned long>(static_cast<u32>(result)));
    return code;
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
    // The 3DS mbedTLS port gathers entropy from the SSL service.
    const Result sslResult = sslcInit(0);
    const bool sslAvailable = R_SUCCEEDED(sslResult);
    handheld::SdStorage files(handheld::kAppDirectory);
    Settings settings;
    std::string notice;
    try {
        std::string saved;
        if (files.read("settings.cfg", saved, 4096)) settings = handheld::parseSettings(saved);
    } catch (const std::exception& error) {
        notice = error.what();
    }
    {
        Worker worker;
        const bool available = networkAvailable && sslAvailable && worker.start();
        HookContext context{&worker};
        aptHookCookie cookie;
        aptHook(&cookie, lifecycle, &context);
        if (!networkAvailable) notice = "Socket service: " + resultCode(networkResult);
        else if (!sslAvailable) notice = "SSL service: " + resultCode(sslResult);
        else if (!available) notice = "Could not start network worker.";
        View view;
        Screen screen;
        int scroll = 0;
        std::uint64_t revision = ~std::uint64_t(0), sentRevision = 0;
        std::string place;
        std::vector<Row> rows;
        std::map<std::string, std::vector<Row>> rowCache;
        const auto save = [&] {
            try {
                files.write("settings.cfg", handheld::encodeSettings(settings));
            } catch (const std::exception& error) {
                notice = std::string(error.what()) + "; settings not saved";
            }
        };
        const auto connect = [&] {
            if (!available) return;
            worker.connect(settings);
            screen.muted = screen.deafened = false;
            screen.tab = Communities;
        };
        const auto edit = [&](const char* hint, std::string& value, int limit, SwkbdType type) {
            return keyboard(worker, context, screen.muted, screen.deafened, settings.pushToTalk,
                            hint, value, limit, type);
        };
        while (aptMainLoop()) {
            hidScanInput();
            const u32 down = hidKeysDown();
            if (down & KEY_START) break;
            worker.snapshot(view);
            if (view.sentRevision != sentRevision) {
                scroll = 0;
                sentRevision = view.sentRevision;
            }
            if (view.revision != revision) {
                const int oldSize = static_cast<int>(rows.size());
                rows = chatRows(view, rowCache);
                if (place != view.community + '/' + view.channel) {
                    place = view.community + '/' + view.channel;
                    scroll = 0;
                } else if (scroll > 0) scroll += std::max(0, static_cast<int>(rows.size()) - oldSize);
                revision = view.revision;
            }
            const auto* community = view.started ? findCommunity(view, view.community) : nullptr;
            const auto* channel = findChannel(community, view.channel);
            const int tab = screen.tab;
            if (down & KEY_SELECT) screen.tab = SettingsTab;
            if (down & KEY_B) { screen.tab = Chat; notice.clear(); }
            const bool list = screen.tab == Communities || screen.tab == Channels || screen.tab == Members;
            if (down & KEY_UP) { if (list) --screen.list[screen.tab]; else scroll += 3; }
            if (down & KEY_DOWN) { if (list) ++screen.list[screen.tab]; else scroll -= 3; }
            if (down & KEY_R) scroll = 0;
            scroll = std::clamp(scroll, 0, std::max(0, static_cast<int>(rows.size()) - kChatRows));
            for (int i = 0; i < TabCount; ++i)
                screen.list[i] = std::clamp(screen.list[i], 0, std::max(0, listSize(view, i) - kListRows));
            bool compose = false, toggleVoice = down & KEY_Y;
            if ((down & KEY_A) && !view.started && (screen.tab == Communities || screen.tab == Chat)) connect();
            else if ((down & KEY_A) && screen.tab == Chat) compose = true;
            if (down & KEY_X) screen.muted = !screen.muted;
            if (down & KEY_TOUCH) {
                touchPosition touch;
                hidTouchRead(&touch);
                const int first = screen.list[screen.tab];
                const auto target = leaveTarget(view, screen);
                bool leaveTapped = false;
                if (touch.py < 36) screen.tab = std::min(TabCount - 1, touch.px / kTabWidth);
                else if (screen.tab == Communities && !view.started) {
                    if (hit(touch, 12, 47, 296, 43)) connect();
                } else if (screen.tab == Communities) {
                    for (int i = 0; i < kListRows && first + i < static_cast<int>(view.communities.size()); ++i) {
                        if (!hit(touch, 12, static_cast<int>(listY(i, 42)), 296, 24)) continue;
                        const auto& entry = view.communities[static_cast<std::size_t>(first + i)];
                        screen.chosen = entry.id;
                        if (entry.pending) break;
                        if (entry.id != view.community) worker.select(entry.id);
                        screen.tab = Channels;
                        break;
                    }
                    if (hit(touch, 12, 182, 96, 30)) {
                        std::string link = "disquisition://join/";
                        if (edit("Invite link", link, 1024, SWKBD_TYPE_QWERTY)) {
                            worker.join(link);
                            notice.clear();
                        }
                    } else if (hit(touch, 112, 182, 96, 30)) {
                        try {
                            std::string saved;
                            if (!files.read("invite.txt", saved, 8192))
                                notice = "No invite.txt in " + std::string(handheld::kAppDirectory) + ".";
                            else {
                                // Only the first line, without a byte order mark from Windows editors.
                                if (saved.rfind("\xEF\xBB\xBF", 0) == 0) saved.erase(0, 3);
                                saved = handheld::trim(saved);
                                const auto link = handheld::trim(saved.substr(0, saved.find_first_of("\r\n")));
                                if (link.empty()) notice = "invite.txt is empty.";
                                else {
                                    worker.join(link);
                                    notice.clear();
                                }
                            }
                        } catch (const std::exception& error) {
                            notice = error.what();
                        }
                    } else if (hit(touch, 212, 182, 96, 30) && !target.empty()) {
                        leaveTapped = true;
                        if (screen.confirm != target) screen.confirm = target;
                        else {
                            worker.leave(target);
                            screen.confirm.clear();
                            screen.chosen.clear();
                        }
                    }
                } else if (screen.tab == Channels && community) {
                    for (int i = 0; i < kListRows && first + i < static_cast<int>(community->channels.size()); ++i) {
                        if (!hit(touch, 12, static_cast<int>(listY(i, 64)), 296, 24)) continue;
                        const auto& entry = community->channels[static_cast<std::size_t>(first + i)];
                        if (entry.type == "text") {
                            if (entry.id != view.channel) worker.select(community->id, entry.id);
                            screen.tab = Chat;
                        } else if (view.voiceCommunity == community->id && view.voiceChannel == entry.id) {
                            worker.voice({}, {});
                        } else {
                            worker.voice(community->id, entry.id);
                        }
                        break;
                    }
                } else if (screen.tab == Chat) {
                    if (hit(touch, 12, 47, 296, 43)) {
                        if (!view.started) connect();
                        else if (channel) compose = true;
                        else screen.tab = community ? Channels : Communities;
                    } else if (hit(touch, 12, 102, 296, 35)) toggleVoice = true;
                    else if (hit(touch, 12, 149, 142, 33)) screen.muted = !screen.muted;
                    else if (hit(touch, 166, 149, 142, 33)) screen.deafened = !screen.deafened;
                } else if (screen.tab == SettingsTab) {
                    if (hit(touch, 12, 197, 296, 29)) {
                        if (view.started) worker.disconnect();
                        else connect();
                    } else if (hit(touch, 12, 158, 296, 31)) {
                        settings.pushToTalk = !settings.pushToTalk;
                        save();
                    } else if (view.started && touch.py >= 44 && touch.py < 151) {
                        notice = "Disconnect to change your name, relay or color.";
                    } else if (hit(touch, 12, 44, 296, 31)) {
                        std::string value = settings.name;
                        if (edit("Your name", value, 64, SWKBD_TYPE_NORMAL)) {
                            if (!handheld::validName(value)) notice = "Enter a name of at most 64 characters.";
                            else { settings.name = value; save(); }
                        }
                    } else if (hit(touch, 12, 82, 296, 31)) {
                        std::string value = settings.relay;
                        if (edit("Relay host, optionally host:port", value, 260, SWKBD_TYPE_QWERTY)) {
                            if (!handheld::validRelay(value)) notice = "Enter a relay hostname with an optional port.";
                            else { settings.relay = value; save(); }
                        }
                    } else if (hit(touch, 12, 120, 296, 31)) {
                        const auto found = std::find_if(handheld::kColorNames.begin(), handheld::kColorNames.end(),
                            [&](const char* name) { return settings.color == name; });
                        const auto index = found == handheld::kColorNames.end() ? 0 :
                            (static_cast<std::size_t>(found - handheld::kColorNames.begin()) + 1) %
                            handheld::kColorNames.size();
                        settings.color = handheld::kColorNames[index];
                        save();
                    }
                }
                if (!leaveTapped) screen.confirm.clear();
            }
            if (screen.tab != tab) screen.confirm.clear();
            if (compose && channel) {
                std::string body;
                const auto hint = "Message #" + channel->name;
                if (edit(hint.c_str(), body, 500, SWKBD_TYPE_NORMAL)) {
                    worker.chat(body);
                    scroll = 0;
                    notice.clear();
                }
            }
            if (toggleVoice && view.started) {
                const auto* voice = firstVoice(community);
                if (!view.voiceChannel.empty()) worker.voice({}, {});
                else if (voice) worker.voice(community->id, voice->id);
                notice.clear();
            }
            if (available)
                worker.controls(screen.muted, screen.deafened, settings.pushToTalk, hidKeysHeld() & KEY_L);
            C2D_TextBufClear(textBuffer);
            C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
            topScreen(top, view, rows, scroll, notice);
            bottomScreen(bottom, settings, view, screen, available);
            C3D_FrameEnd(0);
        }
        aptUnhook(&cookie);
    } // Stop the worker and release MIC/NDSP before tearing down sockets/graphics.
    if (sslAvailable) sslcExit();
    if (networkAvailable) socExit();
    std::free(socketMemory);
    C2D_TextBufDelete(textBuffer);
    C2D_Fini(); C3D_Fini(); gfxExit();
    return 0;
}
