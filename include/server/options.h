#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace chat {

struct ServerOptions {
    std::uint16_t port = 9000;
    std::string databasePath = "chat.db";
    std::size_t historyLimit = 50;
    std::size_t maxClients = 128;
};

// Prints a message and exits on invalid input or --help.
ServerOptions parseServerOptions(int argc, char** argv);

} // namespace chat
