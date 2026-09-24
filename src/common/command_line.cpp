#include "common/command_line.h"

#include <cstdio>
#include <cstdlib>

#include "common/protocol.h"

namespace chat {

CommandLine::CommandLine(int argc, char** argv, const char* usage) :
    argc_(argc), argv_(argv), usage_(usage) {
}

bool CommandLine::next() {
    if (index_ + 1 >= argc_) {
        return false;
    }
    option_ = argv_[++index_];
    return true;
}

bool CommandLine::is(std::string_view name) const {
    return option_ == name;
}

bool CommandLine::is(std::string_view shortName, std::string_view longName) const {
    return option_ == shortName || option_ == longName;
}

std::string CommandLine::value() {
    if (index_ + 1 >= argc_) {
        fail("missing value for " + option_);
    }
    return argv_[++index_];
}

std::uint16_t CommandLine::port(bool allowZero) {
    std::uint16_t port = 0;
    if (!parsePort(value(), port, allowZero)) {
        fail("port out of range");
    }
    return port;
}

void CommandLine::showHelp() const {
    std::fputs(usage_, stdout);
    std::exit(0);
}

void CommandLine::rejectOption() const {
    std::fprintf(stderr, "unknown option: %s\n", option_.c_str());
    std::fputs(usage_, stderr);
    std::exit(1);
}

void CommandLine::fail(const std::string& message) const {
    std::fprintf(stderr, "invalid arguments: %s\n", message.c_str());
    std::exit(1);
}

} // namespace chat
