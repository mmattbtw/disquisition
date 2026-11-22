#pragma once

#include <cstddef>
#include <string>

namespace wiiu {

// The desktop's named chat colors, in its order.
constexpr const char* kColors[] = {"pink", "mint", "butter", "periwinkle", "lilac", "aqua", "peach"};
constexpr std::size_t kColorCount = sizeof(kColors) / sizeof(kColors[0]);

struct Settings {
    std::string name = "wiiu";
    std::string relay = "relay.disquisition.app";
    std::string color = "mint";
    bool pushToTalk = true;
};

std::string trim(const std::string& text);
bool validName(const std::string& name);
bool validRelay(const std::string& relay);
// Accepts what desktop clients send: a named color, #RRGGBB, or an xterm index.
bool validColor(const std::string& color);
Settings parseSettings(const std::string& text);
std::string encodeSettings(const Settings& settings);
// saved is true when a settings file was found.
Settings loadSettings(const std::string& directory, std::string& notice, bool& saved);
bool saveSettings(const std::string& directory, const Settings& settings, std::string& notice);
// The trimmed first line of an invite.txt file.
bool readInvite(const std::string& path, std::string& link, std::string& notice);

} // namespace wiiu
