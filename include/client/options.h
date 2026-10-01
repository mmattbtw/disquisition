#pragma once

#include <cstdint>
#include <optional>
#include <string>

namespace chat {

struct ClientOptions {
    std::string name; // signs in automatically when set
    std::string host = "127.0.0.1";
    std::uint16_t port = 9000;
    std::uint16_t peerPort = 0; // 0 picks a free port
    std::string advertiseHost;  // address peers should dial instead of the one the server sees

    // With a relay, the relay owns this user's peer links and all chat flows
    // through the one relay connection.
    bool useRelay = false;
    std::string relayHost;
    std::uint16_t relayPort = 0;
    // While the relay is down, connect straight to the server and the mesh.
    bool leakMyIp = false;

    std::string logFile; // empty: no logging, since console output would corrupt the screen
    std::string messageFile; // empty: do not save chat locally
    std::optional<std::int64_t> maxSavedMessages; // absent: unlimited
};

// Reads the command line, or asks interactively when there are no arguments.
// Prints a message and exits on invalid input or --help.
ClientOptions parseClientOptions(int argc, char** argv);

} // namespace chat
