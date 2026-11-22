#include "settings.h"
#include "storage.h"

#include "console/model.h"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <sstream>
#include <sys/stat.h>

namespace wiiu {

std::string trim(const std::string& text) {
    const auto first = text.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return {};
    return text.substr(first, text.find_last_not_of(" \t\r\n") - first + 1);
}

bool validName(const std::string& name) {
    return name == trim(name) && console::label(name, 64);
}

bool validRelay(const std::string& relay) {
    if (relay.empty() || relay.size() > 260) return false;
    for (const unsigned char c : relay) if (c <= ' ' || c >= 127) return false;
    try {
        return !console::normalizeRelayEndpoint(relay).empty();
    } catch (const std::exception&) {
        return false;
    }
}

bool validColor(const std::string& color) {
    for (const auto* name : kColors) if (color == name) return true;
    const auto hex = [](char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
    };
    if (color.size() == 7 && color[0] == '#') {
        for (std::size_t i = 1; i < 7; ++i) if (!hex(color[i])) return false;
        return true;
    }
    if (color.empty() || color.size() > 3) return false;
    for (const char c : color) if (c < '0' || c > '9') return false;
    const int index = std::stoi(color);
    return index <= 255 && index != 1 && index != 2 && index != 250;
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
        else if (key == "push_to_talk" && (value == "0" || value == "1"))
            settings.pushToTalk = value == "1";
    }
    return settings;
}

std::string encodeSettings(const Settings& settings) {
    return "name=" + settings.name + "\nrelay=" + settings.relay + "\ncolor=" + settings.color +
        "\npush_to_talk=" + (settings.pushToTalk ? "1\n" : "0\n");
}

namespace {
bool readFile(const std::string& path, std::string& text, std::size_t limit) {
    FILE* file = std::fopen(path.c_str(), "rb");
    if (!file) return false;
    std::string buffer(limit + 1, '\0');
    const auto size = std::fread(&buffer[0], 1, buffer.size(), file);
    const bool valid = !std::ferror(file) && size <= limit;
    const bool closed = std::fclose(file) == 0;
    if (!valid || !closed) return false;
    text = buffer.substr(0, size);
    return true;
}
bool failed(const char* operation, std::string& notice) {
    notice = std::string("SD ") + operation + ": " + std::strerror(errno);
    return false;
}
}

Settings loadSettings(const std::string& directory, std::string& notice, bool& saved) {
    saved = false;
    if (directory.empty()) {
        notice = "SD card unavailable. Settings will last for this session.";
        return {};
    }
    std::string text;
    if (readFile(directory + "/settings.cfg", text, 4096)) {
        saved = true;
        return parseSettings(text);
    }
    if (readFile(directory + "/settings.cfg.bak", text, 4096)) {
        saved = true;
        notice = "Recovered settings from SD backup.";
        return parseSettings(text);
    }
    return {};
}

bool saveSettings(const std::string& folder, const Settings& settings, std::string& notice) {
    if (folder.empty()) { notice = "SD card unavailable. Settings were not saved."; return false; }
    if (!createDirectories(folder)) return failed("create app folder", notice);
    const std::string path = folder + "/settings.cfg";
    const std::string temporary = path + ".tmp", backup = path + ".bak";
    const std::string text = encodeSettings(settings);
    FILE* file = std::fopen(temporary.c_str(), "wb");
    if (!file) return failed("open settings", notice);
    bool okay = std::fwrite(text.data(), 1, text.size(), file) == text.size();
    if (okay) okay = std::fflush(file) == 0;
    if (std::fclose(file) != 0) okay = false;
    if (!okay) { std::remove(temporary.c_str()); return failed("write settings", notice); }
    struct stat info{};
    const bool existing = ::stat(path.c_str(), &info) == 0;
    if (existing) {
        if (std::remove(backup.c_str()) != 0 && errno != ENOENT) return failed("remove backup", notice);
        if (std::rename(path.c_str(), backup.c_str()) != 0) return failed("backup settings", notice);
    }
    if (std::rename(temporary.c_str(), path.c_str()) != 0) {
        if (existing) std::rename(backup.c_str(), path.c_str());
        return failed("replace settings", notice);
    }
    notice = "Settings saved to SD.";
    return true;
}

bool readInvite(const std::string& path, std::string& link, std::string& notice) {
    std::string text;
    if (path.empty() || !readFile(path, text, 8192)) {
        notice = "Could not read sd:/wiiu/apps/disquisition/invite.txt.";
        return false;
    }
    if (text.compare(0, 3, "\xEF\xBB\xBF") == 0) text.erase(0, 3);
    text = trim(text);
    link = trim(text.substr(0, text.find_first_of("\r\n")));
    if (link.empty()) {
        notice = "invite.txt is empty.";
        return false;
    }
    return true;
}

} // namespace wiiu
