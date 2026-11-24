#include "settings.h"
#include "console/model.h"

#include <algorithm>
#include <cctype>
#include <exception>
#include <sstream>

namespace handheld {

std::string trim(const std::string& text) {
    const auto first = text.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return {};
    return text.substr(first, text.find_last_not_of(" \t\r\n") - first + 1);
}

bool validName(const std::string& name) { return console::label(name, 64); }

bool validRelay(const std::string& relay) {
    try {
        return !console::normalizeRelayEndpoint(relay).empty();
    } catch (const std::exception&) {
        return false;
    }
}

bool validColor(const std::string& color) {
    for (const char* name : kColorNames)
        if (color == name) return true;
    if (color.size() == 7 && color[0] == '#')
        return std::all_of(color.begin() + 1, color.end(),
                           [](char ch) { return std::isxdigit(static_cast<unsigned char>(ch)) != 0; });
    return !color.empty() && color.size() <= 3 &&
        std::all_of(color.begin(), color.end(), [](char ch) { return ch >= '0' && ch <= '9'; }) &&
        std::stoi(color) <= 255;
}

Settings parseSettings(const std::string& text) {
    Settings settings;
    std::istringstream file(text);
    std::string line;
    while (std::getline(file, line)) {
        const auto separator = line.find('=');
        if (separator == std::string::npos) continue;
        const auto key = trim(line.substr(0, separator));
        const auto value = trim(line.substr(separator + 1));
        if (key == "name" && validName(value)) settings.name = value;
        else if (key == "relay" && validRelay(value)) settings.relay = value;
        else if (key == "color" && validColor(value)) settings.color = value;
        else if (key == "push_to_talk") settings.pushToTalk = value != "0";
    }
    return settings;
}

std::string encodeSettings(const Settings& settings) {
    return "name=" + settings.name + "\nrelay=" + settings.relay + "\ncolor=" + settings.color +
        "\npush_to_talk=" + (settings.pushToTalk ? "1\n" : "0\n");
}

} // namespace handheld
