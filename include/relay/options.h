#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace chat {

struct RelayOptions {
    std::string serverHost = "127.0.0.1";
    std::uint16_t serverPort = 9000;
    // The public host peers dial to reach this relay's users. Without it the
    // server only knows the address the relay connects from.
    std::string advertiseHost;
    // Shared by every hosted user, for their clients and their peers alike.
    std::uint16_t port = 3333;
    std::size_t maxUsers = 64;
};

// Prints a message and exits on invalid input or --help.
RelayOptions parseRelayOptions(int argc, char** argv);

} // namespace chat
