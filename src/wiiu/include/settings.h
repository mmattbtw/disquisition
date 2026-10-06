#pragma once

#include <cstdint>
#include <string>

namespace wiiu {

struct Settings {
    std::string host = "relay.mmatt.net";
    std::uint16_t port = 3333;
    std::string name = "wiiu";
    std::string color = "mint";
    bool pushToTalk = true;
};

bool validHost(const std::string& host);
Settings parseSettings(const std::string& text);
std::string encodeSettings(const Settings& settings);
Settings loadSettings(const std::string& directory, std::string& notice);
bool saveSettings(const std::string& directory, const Settings& settings, std::string& notice);

} // namespace wiiu
