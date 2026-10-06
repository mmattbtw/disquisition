#include "settings.h"
#include "common/protocol.h"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <sstream>
#include <sys/stat.h>

namespace wiiu {

bool validHost(const std::string& host) {
    if (host.empty() || host.size() > 253) return false;
    for (unsigned char c : host) {
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '-' || c == '.')) return false;
    }
    return true;
}

Settings parseSettings(const std::string& text) {
    Settings settings;
    std::istringstream file(text);
    std::string line;
    while (std::getline(file, line)) {
        const auto separator = line.find('=');
        if (separator == std::string::npos) continue;
        const auto key = chat::trim(line.substr(0, separator));
        const auto value = chat::trim(line.substr(separator + 1));
        if (key == "host" && validHost(value)) settings.host = value;
        else if (key == "port") {
            std::uint16_t port;
            if (chat::parsePort(value, port, false)) settings.port = port;
        } else if (key == "name") {
            const auto name = chat::sanitizeName(value);
            if (!name.empty()) settings.name = name;
        } else if (key == "color" && chat::isValidColor(value)) settings.color = value;
        else if (key == "push_to_talk" && (value == "0" || value == "1"))
            settings.pushToTalk = value == "1";
    }
    return settings;
}

std::string encodeSettings(const Settings& settings) {
    return "host=" + settings.host + "\nport=" + std::to_string(settings.port) +
        "\nname=" + chat::sanitizeName(settings.name) + "\ncolor=" + settings.color +
        "\npush_to_talk=" + (settings.pushToTalk ? "1\n" : "0\n");
}

namespace {
bool readFile(const std::string& path, std::string& text) {
    FILE* file = std::fopen(path.c_str(), "rb");
    if (!file) return false;
    char buffer[4097];
    const auto size = std::fread(buffer, 1, sizeof(buffer), file);
    const bool valid = !std::ferror(file) && size <= 4096;
    const bool closed = std::fclose(file) == 0;
    if (!valid || !closed) return false;
    text.assign(buffer, size);
    return true;
}
bool directory(const std::string& path) {
    if (::mkdir(path.c_str(), 0777) == 0) return true;
    struct stat info{};
    return errno == EEXIST && ::stat(path.c_str(), &info) == 0 && S_ISDIR(info.st_mode);
}
bool failed(const char* operation, std::string& notice) {
    notice = std::string("SD ") + operation + ": " + std::strerror(errno);
    return false;
}
}

Settings loadSettings(const std::string& directory, std::string& notice) {
    if (directory.empty()) {
        notice = "SD card unavailable. Settings will last for this session.";
        return {};
    }
    std::string text;
    if (readFile(directory + "/settings.cfg", text)) return parseSettings(text);
    if (readFile(directory + "/settings.cfg.bak", text)) {
        notice = "Recovered settings from SD backup.";
        return parseSettings(text);
    }
    return {};
}

bool saveSettings(const std::string& folder, const Settings& settings, std::string& notice) {
    if (folder.empty()) { notice = "SD card unavailable. Settings were not saved."; return false; }
    // Create every component under the already-mounted SD root.
    const auto appRoot = folder.find("/wiiu/");
    const auto first = appRoot == std::string::npos ? std::size_t(1) : appRoot + 1;
    for (std::size_t end = folder.find('/', first); end != std::string::npos;
         end = folder.find('/', end + 1)) {
        if (!directory(folder.substr(0, end))) return failed("create folder", notice);
    }
    if (!directory(folder)) return failed("create app folder", notice);
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

} // namespace wiiu
