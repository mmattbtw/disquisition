#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace chat {

// Walks argv for the option parsers. Errors print a message and exit, so a
// parser is just a flat if/else over the options.
class CommandLine {
public:
    CommandLine(int argc, char** argv, const char* usage);

    // Moves to the next option. False when none are left.
    bool next();
    bool is(std::string_view name) const;
    bool is(std::string_view shortName, std::string_view longName) const;

    // The argument following the current option.
    std::string value();
    std::uint16_t port(bool allowZero = false);

    [[noreturn]] void showHelp() const;
    [[noreturn]] void rejectOption() const;
    [[noreturn]] void fail(const std::string& message) const;

private:
    int argc_;
    char** argv_;
    const char* usage_;
    int index_ = 0;
    std::string option_;
};

} // namespace chat
