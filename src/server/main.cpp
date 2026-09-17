#include <cstdio>
#include <cstdlib>
#include <exception>
#include <stdexcept>
#include <string>

#include "server/database.h"
#include "server/server.h"

namespace {

void printUsage(std::FILE* out) {
    std::fprintf(out,
                 "usage: server [options]\n"
                 "\n"
                 "  -p, --port <port>        port to listen on (default 9000)\n"
                 "  -d, --db <path>          sqlite database file (default chat.db)\n"
                 "      --history <count>    messages replayed to new clients (default 50)\n"
                 "  -h, --help               show this message\n");
}

}  // namespace

int main(int argc, char** argv) {
    chat::ServerOptions options;

    for (int index = 1; index < argc; ++index) {
        const std::string argument = argv[index];
        try {
            if (argument == "-p" || argument == "--port") {
                if (index + 1 >= argc) {
                    throw std::runtime_error("missing value for " + argument);
                }
                const int port = std::stoi(argv[++index]);
                if (port < 0 || port > 65535) {
                    throw std::runtime_error("port out of range");
                }
                options.port = static_cast<std::uint16_t>(port);
            } else if (argument == "-d" || argument == "--db") {
                if (index + 1 >= argc) {
                    throw std::runtime_error("missing value for " + argument);
                }
                options.databasePath = argv[++index];
            } else if (argument == "--history") {
                if (index + 1 >= argc) {
                    throw std::runtime_error("missing value for " + argument);
                }
                options.historyLimit = static_cast<std::size_t>(std::stoul(argv[++index]));
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

    try {
        chat::Database database(options.databasePath);
        chat::Server server(options, database);
        server.listen();
        server.run();
    } catch (const std::exception& error) {
        std::fprintf(stderr, "server error: %s\n", error.what());
        return 1;
    }
    return 0;
}
