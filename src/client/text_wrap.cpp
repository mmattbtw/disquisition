#include "client/text_wrap.h"

#include <sstream>

namespace chat {

std::vector<std::string> wrapText(const std::string& prefix, const std::string& body, int width) {
    std::vector<std::string> lines;
    if (width <= 1) {
        lines.push_back(prefix + body);
        return lines;
    }

    std::string indent;
    if (static_cast<int>(prefix.size()) < width) {
        indent.assign(prefix.size(), ' ');
    }

    std::vector<std::string> words;
    std::istringstream stream(body);
    std::string word;
    while (stream >> word) {
        words.push_back(word);
    }
    if (words.empty()) {
        lines.push_back(prefix);
        return lines;
    }

    std::string line = prefix;
    bool fresh = true;
    for (const std::string& original : words) {
        std::string current = original;
        for (;;) {
            if (fresh && line.size() >= static_cast<std::size_t>(width)) {
                line.clear();
            }
            const std::size_t separator = fresh ? 0 : 1;
            const std::size_t used = line.size() + separator;
            const std::size_t room = used < static_cast<std::size_t>(width)
                                         ? static_cast<std::size_t>(width) - used
                                         : 0;
            if (current.size() <= room) {
                if (!fresh) {
                    line += ' ';
                }
                line += current;
                fresh = false;
                break;
            }
            if (!fresh) {
                lines.push_back(line);
                line = indent;
                fresh = true;
                continue;
            }
            const std::size_t take = room > 0 ? room : 1;
            line += current.substr(0, take);
            current.erase(0, take);
            lines.push_back(line);
            line = indent;
            fresh = true;
        }
    }
    if (!line.empty()) {
        lines.push_back(line);
    }
    return lines;
}

} // namespace chat
