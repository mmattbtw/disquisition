#include "render.h"
#include "font8x8_basic.h"

#include <algorithm>

namespace wiiu {

namespace {
// An 8x8 speaker in font8x8's bit order, for voice channels.
constexpr unsigned char speakerGlyph[8] = {0x08, 0x4C, 0x8F, 0xAF, 0xAF, 0x8F, 0x4C, 0x08};
}

void Canvas::pixel(int x, int y, std::uint32_t color) {
    if (x >= 0 && x < width_ && y >= 0 && y < height_) putPixel(x, y, color);
}

void Canvas::glyph(int x, int y, const unsigned char* rows, std::uint32_t color) {
    for (int row = 0; row < 8; ++row) {
        for (int column = 0; column < 8; ++column) {
            if (!(rows[row] & (1u << column))) continue;
            for (int dy = 0; dy < 2; ++dy)
                for (int dx = 0; dx < 2; ++dx)
                    pixel(x + column * 2 + dx, y + row * 2 + dy, color);
        }
    }
}

void Canvas::textAt(int x, int y, const std::string& value, std::size_t columns,
                    std::uint32_t color) {
    const auto visible = displayText(value).substr(0, columns);
    for (const unsigned char c : visible) {
        glyph(x, y, font8x8_basic[c], color);
        x += cellWidth;
    }
}

void Canvas::text(int column, int row, const std::string& value, std::size_t columns,
                  std::uint32_t color) {
    textAt(column * cellWidth, row * cellHeight + 4, value, columns, color);
}

void Canvas::speaker(int column, int row, std::uint32_t color) {
    glyph(column * cellWidth, row * cellHeight + 4, speakerGlyph, color);
}

void Canvas::fill(int left, int top, int right, int bottom, std::uint32_t color) {
    for (int y = top; y < bottom; ++y)
        for (int x = left; x < right; ++x) pixel(x, y, color);
}

void Canvas::highlight(Rect r) {
    fill(r.column * cellWidth - 8, r.row * cellHeight + 1,
         (r.column + r.columns) * cellWidth - 8, (r.row + r.rows) * cellHeight - 1, border);
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
// Truncates after display conversion, so a marker is never cut off.
std::string fitted(const std::string& value, std::size_t columns, const std::string& marker = {}) {
    auto text = displayText(value);
    const auto room = columns > marker.size() ? columns - marker.size() : 0;
    if (text.size() > room) text.resize(room);
    return text + marker;
}

void header(Canvas& canvas, const console::View& view, int columns) {
    const auto* community = selectedCommunity(view);
    const std::string state = !view.started ? "Offline" : view.relayConnected ? "Online" : "Connecting";
    const int stateColumn = columns - 1 - int(state.size());
    canvas.text(1, 0, community ? community->name : "Disquisition", std::size_t(stateColumn - 3));
    canvas.text(stateColumn, 0, state, state.size(), view.relayConnected ? accent : dim);
    canvas.text(1, 1, view.status, std::size_t(columns - 2));
}

void transcript(Canvas& canvas, const Scene& scene, int columns, int first, int rows) {
    const auto lines = chatLines(scene.view.messages, std::size_t(columns - 2));
    const auto maximum = lines.size() > std::size_t(rows) ? lines.size() - rows : 0;
    const auto end = lines.size() - std::min(scene.scroll, maximum);
    const auto start = end > std::size_t(rows) ? end - rows : 0;
    for (std::size_t i = start; i < end; ++i) {
        const int row = first + int(i - start);
        canvas.text(1, row, lines[i].text, std::size_t(columns - 2));
        for (int y = row * cellHeight + 4; y < row * cellHeight + 20; ++y)
            canvas.pixel(5, y, messageColor(lines[i].color));
    }
    if (lines.empty()) {
        const char* empty = !scene.view.started ? "Not connected." :
            scene.view.channel.empty() ? "Choose a text channel." : "No messages yet.";
        canvas.text(1, first, empty, std::size_t(columns - 2), dim);
    }
}

std::string voiceState(const console::View& view) {
    if (view.voiceChannel.empty()) return {};
    std::string text = "Voice: ";
    for (const auto& community : view.communities)
        if (community.id == view.voiceCommunity) text += channelName(community, view.voiceChannel);
    if (view.deafened) text += "  deafened";
    else if (view.muted) text += "  muted";
    else if (view.transmitting) text += "  talking";
    return text;
}

std::string memberState(const console::CommunityView& community, const console::MemberView& member) {
    if (!member.online) return "offline";
    if (member.voiceChannel.empty()) return "online";
    std::string state = channelName(community, member.voiceChannel);
    if (member.deafened) state += ", deafened";
    else if (member.muted) state += ", muted";
    if (member.speaking) state += ", speaking";
    return state;
}

void editor(Canvas& canvas, const Scene& scene) {
    const auto& edit = scene.editor;
    canvas.text(1, 2, edit.title);
    canvas.text(1, 3, scene.notice);
    std::string preview = edit.text;
    preview.insert(edit.cursor(), "|");
    // Scroll whole rows so long invite links keep their line breaks.
    const std::size_t start = edit.cursor() >= 200 ? (edit.cursor() / 50 - 3) * 50 : 0;
    for (int row = 0; row < 4; ++row) {
        const auto index = start + row * 50;
        if (index < preview.size()) canvas.text(1, 4 + row, preview.substr(index, 50));
    }
    for (int i = 0; i < 52; ++i) canvas.button(edit.keyRect(i), edit.key(i), edit.selected == i);
    canvas.text(1, 19, "A key  X delete  B back  + done  L/R cursor  - exit");
}

void communities(Canvas& pad, const Scene& scene) {
    const auto& view = scene.view;
    const auto& list = view.communities;
    if (list.empty())
        pad.text(1, contentRow, view.started ? "No communities. Join with an invite." :
            "Press + to connect.", 51, dim);
    auto first = firstVisible(scene.community, list.size(), contentRows);
    for (int row = 0; row < contentRows && first + row < list.size(); ++row) {
        const auto& entry = list[first + row];
        const auto rect = communityRect(row);
        if (scene.pane == Pane::Communities && first + row == scene.community) pad.highlight(rect);
        const auto color = entry.pending ? dim : entry.id == view.community ? accent : foreground;
        pad.text(rect.column, rect.row, fitted(entry.name, 19, entry.unread ? " *" : ""), 19, color);
    }
    if (list.empty()) return;
    for (int y = contentRow * cellHeight; y < (contentRow + contentRows) * cellHeight; ++y)
        pad.pixel(22 * cellWidth - 8, y, border);
    const auto* community = selectedCommunity(view);
    if (!community) return;
    if (community->channels.empty()) pad.text(channelRect(0).column, contentRow, "No channels yet.", 29, dim);
    first = firstVisible(scene.channel, community->channels.size(), contentRows);
    for (int row = 0; row < contentRows && first + row < community->channels.size(); ++row) {
        const auto& channel = community->channels[first + row];
        const auto rect = channelRect(row);
        if (scene.pane == Pane::Channels && first + row == scene.channel) pad.highlight(rect);
        const bool voice = channel.type == "voice";
        const bool active = voice ? view.voiceCommunity == community->id && view.voiceChannel == channel.id :
            view.channel == channel.id;
        const auto color = active ? accent : foreground;
        std::string marker = channel.unread ? " *" : "";
        if (voice) {
            int people = 0;
            for (const auto& member : community->members)
                if (member.online && member.voiceChannel == channel.id) ++people;
            if (people) marker += "  " + std::to_string(people);
            pad.speaker(rect.column, rect.row, color);
        } else pad.text(rect.column, rect.row, "#", 1, color);
        pad.text(rect.column + 2, rect.row, fitted(channel.name, 27, marker), 27, color);
    }
}

void members(Canvas& pad, const Scene& scene) {
    const auto* community = selectedCommunity(scene.view);
    if (!community) {
        pad.text(1, contentRow, scene.view.started ? "Choose a community." : "Not connected.", 51, dim);
        return;
    }
    const auto& list = community->members;
    const auto maximum = list.size() > std::size_t(contentRows) ? list.size() - contentRows : 0;
    const auto first = std::min(scene.memberScroll, maximum);
    for (int row = 0; row < contentRows && first + row < list.size(); ++row) {
        const auto& member = list[first + row];
        const auto color = !member.online ? dim : member.speaking ? accent : foreground;
        pad.text(1, contentRow + row, fitted(member.name, 23, member.self ? " (you)" : ""), 23, color);
        pad.text(25, contentRow + row, memberState(*community, member), 27, member.online ? foreground : dim);
    }
}

void settings(Canvas& pad, const Scene& scene) {
    const auto& settings = scene.settings;
    const std::string fields[] = {"Name: " + settings.name, "Relay: " + settings.relay,
        "Color: " + settings.color,
        settings.pushToTalk ? "Microphone: hold ZR to talk" : "Microphone: open mic",
        scene.view.started ? "Disconnect" : "Connect"};
    for (int i = 0; i < 5; ++i) pad.button(settingRect(i), fields[i], scene.setting == i);
    const auto swatch = settingRect(2);
    const int top = swatch.row * cellHeight + 15;
    pad.fill(48 * cellWidth, top, 49 * cellWidth, top + 16, messageColor(settings.color));
}
}

void render(const Scene& scene, Canvas& tv, Canvas& gamepad) {
    const auto& view = scene.view;
    header(tv, view, 80);
    const auto* community = selectedCommunity(view);
    if (community && !view.channel.empty()) {
        const std::string heading = "# " + fitted(channelName(*community, view.channel), 40);
        tv.text(1, 2, heading, heading.size(), accent);
        for (const auto& channel : community->channels)
            if (channel.id == view.channel && !channel.topic.empty())
                tv.text(int(heading.size()) + 3, 2, channel.topic, 76 - heading.size(), dim);
    }
    transcript(tv, scene, 80, 3, 24);
    tv.text(1, 27, voiceState(view), 78);
    tv.text(1, 28, scene.notice, 78);

    header(gamepad, view, 53);
    if (scene.editor.active) { editor(gamepad, scene); return; }
    const char* tabs[] = {"Communities", "Chat", "Members", "Settings"};
    for (int i = 0; i < 4; ++i) gamepad.button(tabRect(i), tabs[i], int(scene.tab) == i);
    if (scene.tab == Tab::Settings) settings(gamepad, scene);
    else {
        if (scene.tab == Tab::Communities) {
            communities(gamepad, scene);
            gamepad.button(actionRect(0), "Type invite");
            gamepad.button(actionRect(1), "Use invite.txt");
            gamepad.button(actionRect(2), scene.confirmLeave ? "Confirm leave" : "Leave", scene.confirmLeave);
        } else {
            if (scene.tab == Tab::Chat) transcript(gamepad, scene, 53, contentRow, contentRows);
            else members(gamepad, scene);
            const auto label = !view.started ? std::string("A / tap to connect") :
                !community || view.channel.empty() ? std::string("Choose a text channel") :
                "Message #" + channelName(*community, view.channel);
            gamepad.button(composeRect(), label);
        }
        gamepad.button(voiceRect(0), scene.voice ? "Leave voice" : "Join voice", scene.voice);
        gamepad.button(voiceRect(1), scene.muted ? "Unmute" : "Mute", scene.muted);
        gamepad.button(voiceRect(2), scene.deafened ? "Undeafen" : "Deafen", scene.deafened);
    }
    gamepad.text(1, 18, scene.notice);
    gamepad.text(1, 19, "L/R tabs  + connect  Y voice  ZR talk  - exit");
}

} // namespace wiiu
