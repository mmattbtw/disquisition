#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <stdexcept>
#include <string>
#include <utility>

#include "relay/relay.h"

namespace {

void printUsage(std::FILE* out) {
    std::fprintf(out,
                 "usage: relay --advertise <host> [options]\n"
                 "\n"
                 "  -H, --host <host>       chat server host (default 127.0.0.1)\n"
                 "  -p, --port <port>       chat server port (default 9000)\n"
                 "      --advertise <host>  public host other peers should dial to reach\n"
                 "                          this relay (e.g. relay.example.net). Strongly\n"
                 "                          recommended: it is what lets unreachable\n"
                 "                          clients be discovered and dialled\n"
                 "      --listen <port>     the one port every hosted user shares, for\n"
                 "                          clients and peers alike (default 3333)\n"
                 "  -h, --help              show this message\n"
                 "\n"
                 "Names come from the clients: any number of clients may attach, each\n"
                 "signing in as the name it supplies. Point a client at it with:\n"
                 "  client --relay <advertise-host>:<listen>\n");
}

}  // namespace

int main(int argc, char** argv) {
    chat::RelayOptions options;

    for (int index = 1; index < argc; ++index) {
        const std::string argument = argv[index];
        const bool hasValue = index + 1 < argc;
        try {
            if (argument == "-H" || argument == "--host") {
                if (!hasValue) {
                    throw std::runtime_error("missing value for " + argument);
                }
                options.serverHost = argv[++index];
            } else if (argument == "-p" || argument == "--port") {
                if (!hasValue) {
                    throw std::runtime_error("missing value for " + argument);
                }
                if (!chat::parsePort(argv[++index], options.serverPort, false)) {
                    throw std::runtime_error("port out of range");
                }
            } else if (argument == "--advertise") {
                if (!hasValue) {
                    throw std::runtime_error("missing value for " + argument);
                }
                options.advertiseHost = argv[++index];
            } else if (argument == "--listen") {
                if (!hasValue) {
                    throw std::runtime_error("missing value for " + argument);
                }
                if (!chat::parsePort(argv[++index], options.port, true)) {
                    throw std::runtime_error("port out of range");
                }
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

    chat::Relay relay(std::move(options));
    return relay.run();
}
