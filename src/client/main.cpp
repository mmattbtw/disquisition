#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <stdexcept>
#include <string>

#include "client/connection.h"
#include "client/peer_network.h"
#include "client/tui.h"
#include "common/protocol.h"

namespace {

void printUsage(std::FILE* out) {
    std::fprintf(out,
                 "usage: client [options]\n"
                 "\n"
                 "  -H, --host <host>    server host (default 127.0.0.1)\n"
                 "  -p, --port <port>    server port (default 9000)\n"
                 "  -n, --name <name>    sign in automatically with this name\n"
                 "      --p2p-port <port>  listen port for direct peer connections\n"
                 "                       (default 0, which picks a free port)\n"
                 "      --advertise <host> address to tell peers to dial for p2p, instead of\n"
                 "                       the address the server sees you connect from. Use\n"
                 "                       your public IP or hostname to accept peers over the\n"
                 "                       internet (pair with --p2p-port + a port forward)\n"
                 "  -h, --help           show this message\n");
}

}  // namespace

int main(int argc, char** argv) {
    std::string host = "127.0.0.1";
    std::uint16_t port = 9000;
    std::uint16_t peerPort = 0;
    std::string advertise;
    std::string name;

    for (int index = 1; index < argc; ++index) {
        const std::string argument = argv[index];
        const bool hasValue = index + 1 < argc;
        try {
            if (argument == "-H" || argument == "--host") {
                if (!hasValue) {
                    throw std::runtime_error("missing value for " + argument);
                }
                host = argv[++index];
            } else if (argument == "-p" || argument == "--port") {
                if (!hasValue) {
                    throw std::runtime_error("missing value for " + argument);
                }
                const int value = std::stoi(argv[++index]);
                if (value < 0 || value > 65535) {
                    throw std::runtime_error("port out of range");
                }
                port = static_cast<std::uint16_t>(value);
            } else if (argument == "--p2p-port") {
                if (!hasValue) {
                    throw std::runtime_error("missing value for " + argument);
                }
                const int value = std::stoi(argv[++index]);
                if (value < 0 || value > 65535) {
                    throw std::runtime_error("port out of range");
                }
                peerPort = static_cast<std::uint16_t>(value);
            } else if (argument == "--advertise") {
                if (!hasValue) {
                    throw std::runtime_error("missing value for " + argument);
                }
                advertise = argv[++index];
            } else if (argument == "-n" || argument == "--name") {
                if (!hasValue) {
                    throw std::runtime_error("missing value for " + argument);
                }
                name = argv[++index];
            } else if (argument == "-h" || argument == "--help") {
                printUsage(stdout);
                return 0;
            } else {
                std::fprintf(stderr, "unknown option: %s\n", argument.c_str());
                printUsage(stderr);
                return 1;
            }
        } catch (const std::exception& error) {
            std::fprintf(stderr, "invalid arguments: %s\n", error.what());
            return 1;
        }
    }

    // A peer closing the socket mid-write must not take the client down.
    std::signal(SIGPIPE, SIG_IGN);

    // The peer listener must be up before we announce its port to the server.
    chat::PeerNetwork peers;
    std::string error;
    if (!peers.start(peerPort, error)) {
        std::fprintf(stderr, "cannot listen for peers on port %u: %s\n", peerPort, error.c_str());
        return 1;
    }

    chat::Connection connection;
    if (!connection.connectTo(host, port, 5000, error)) {
        std::fprintf(stderr, "cannot connect to %s:%u: %s; retrying in the background\n", host.c_str(),
                     port, error.c_str());
    } else {
        connection.startReader();
    }

    chat::Tui tui(connection, peers);
    return tui.run(name, host, port, advertise);
}
