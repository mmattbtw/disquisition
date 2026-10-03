#pragma once

#include <cstdint>
#include <string>

namespace handheld {

struct Settings {
    std::string host;
    std::uint16_t port = 3333;
    std::string name = "3ds";
    std::string color = "mint";
    bool pushToTalk = true;
};

Settings parseSettings(const std::string& text);
std::string encodeSettings(const Settings& settings);

} // namespace handheld
