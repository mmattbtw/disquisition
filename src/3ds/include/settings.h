#pragma once

#include <array>
#include <string>

namespace handheld {

// The desktop's named chat colors, in its order.
constexpr std::array<const char*, 7> kColorNames{"pink", "mint", "butter", "periwinkle",
                                                 "lilac", "aqua", "peach"};

struct Settings {
    std::string name = "3ds";
    std::string relay = "relay.disquisition.app";
    std::string color = "mint";
    bool pushToTalk = true;
};

Settings parseSettings(const std::string& text);
std::string encodeSettings(const Settings& settings);
bool validName(const std::string& name);
bool validRelay(const std::string& relay);
// A named color, #RRGGBB, or an xterm index from 0 to 255, like the desktop.
bool validColor(const std::string& color);
std::string trim(const std::string& text);

} // namespace handheld
