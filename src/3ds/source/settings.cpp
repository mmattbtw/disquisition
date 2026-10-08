#include "settings.h"
#include "common/protocol.h"

#include <sstream>

namespace handheld {

Settings parseSettings(const std::string& text) {
    Settings settings;
    std::istringstream file(text);
    std::string line;
    while (std::getline(file, line)) {
        const auto separator = line.find('=');
        if (separator == std::string::npos) continue;
        const auto key = chat::trim(line.substr(0, separator));
        const auto value = chat::trim(line.substr(separator + 1));
        if (key == "host" && value.size() <= 253 &&
            value.find_first_of(" \t\r\n/=:") == std::string::npos) settings.host = value;
        else if (key == "port") chat::parsePort(value, settings.port, false);
        else if (key == "name") settings.name = chat::sanitizeName(value);
        else if (key == "color" && chat::isValidColor(value)) settings.color = value;
        else if (key == "push_to_talk") settings.pushToTalk = value != "0";
    }
    if (settings.name.empty()) settings.name = "3ds";
    return settings;
}

std::string encodeSettings(const Settings& settings) {
    return "host=" + settings.host + "\nport=" + std::to_string(settings.port) +
        "\nname=" + chat::sanitizeName(settings.name) + "\ncolor=" + settings.color +
        "\npush_to_talk=" + (settings.pushToTalk ? "1\n" : "0\n");
}

} // namespace handheld
