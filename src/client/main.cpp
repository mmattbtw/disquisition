#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <iostream>
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
                 "      --relay <host[:port]>  reach the mesh through a relay instead of\n"
                 "                       accepting direct connections. Your traffic and\n"
                 "                       everyone else's is tunnelled through it, so no\n"
                 "                       port forward is needed (default port 42069)\n"
                 "  -h, --help           show this message\n");
}

// Splits "host", "host:port" or "[v6]:port" into its parts. A bare host keeps
// the supplied default port.
bool splitHostPort(const std::string& text, std::string& host, std::uint16_t& port,
                   std::uint16_t defaultPort) {
    if (text.empty()) {
        return false;
    }
    if (text.front() == '[') {
        const auto close = text.find(']');
        if (close == std::string::npos || close == 1) {
            return false;
        }
        host = text.substr(1, close - 1);
        if (close + 1 < text.size()) {
            if (text[close + 1] != ':') {
                return false;
            }
            if (!chat::parsePort(text.substr(close + 2), port, false)) {
                return false;
            }
        } else {
            port = defaultPort;
        }
        return true;
    }
    const auto colon = text.rfind(':');
    if (colon == std::string::npos) {
        host = text;
        port = defaultPort;
        return true;
    }
    if (text.find(':') != colon) {
        return false;
    }
    host = text.substr(0, colon);
    if (host.empty() || !chat::parsePort(text.substr(colon + 1), port, false)) {
        return false;
    }
    return true;
}

std::string trim(const std::string& text) {
    const auto begin = text.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos) {
        return "";
    }
    const auto end = text.find_last_not_of(" \t\r\n");
    return text.substr(begin, end - begin + 1);
}

bool promptForConnection(std::string& host, std::uint16_t& port, std::string& name) {
    host = "relay.mmatt.net";
    port = 9000;

    std::string answer;
    std::cout << "server host [relay.mmatt.net]: " << std::flush;
    if (!std::getline(std::cin, answer)) {
        return false;
    }
    answer = trim(answer);
    if (!answer.empty()) {
        host = answer;
    }

    for (;;) {
        std::cout << "server port [9000]: " << std::flush;
        if (!std::getline(std::cin, answer)) {
            return false;
        }
        answer = trim(answer);
        if (answer.empty()) {
            break;
        }
        if (chat::parsePort(answer, port, false)) {
            break;
        }
        std::cout << "please enter a port from 1 to 65535\n";
    }

    for (;;) {
        std::cout << "name: " << std::flush;
        if (!std::getline(std::cin, answer)) {
            return false;
        }
        name = chat::sanitizeName(answer);
        if (!name.empty()) {
            break;
        }
        std::cout << "please enter a name\n";
    }
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    std::string host = "127.0.0.1";
    std::uint16_t port = 9000;
    std::uint16_t peerPort = 0;
    std::string advertise;
    std::string name;
    std::string relayText;

    if (argc == 1 && !promptForConnection(host, port, name)) {
        std::fprintf(stderr, "setup cancelled before connecting\n");
        return 1;
    }

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
                if (!chat::parsePort(argv[++index], port, false)) {
                    throw std::runtime_error("port out of range");
                }
            } else if (argument == "--p2p-port") {
                if (!hasValue) {
                    throw std::runtime_error("missing value for " + argument);
                }
                if (!chat::parsePort(argv[++index], peerPort, true)) {
                    throw std::runtime_error("port out of range");
                }
            } else if (argument == "--advertise") {
                if (!hasValue) {
                    throw std::runtime_error("missing value for " + argument);
                }
                advertise = argv[++index];
            } else if (argument == "--relay") {
                if (!hasValue) {
                    throw std::runtime_error("missing value for " + argument);
                }
                relayText = argv[++index];
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

    const bool useRelay = !relayText.empty();
    std::string relayHost;
    std::uint16_t relayPort = 0;
    if (useRelay) {
        try {
            if (!splitHostPort(relayText, relayHost, relayPort, 42069)) {
                throw std::runtime_error("expected host or host:port");
            }
        } catch (const std::exception& error) {
            std::fprintf(stderr, "invalid --relay value: %s\n", error.what());
            return 1;
        }
    }

    // The peer listener must be up before we announce its port to the server.
    // Through a relay we never listen: the relay is our public peer address.
    chat::PeerNetwork peers;
    std::string error;
    if (useRelay) {
        peers.setPassive(true);
    } else if (!peers.start(peerPort, error)) {
        std::fprintf(stderr, "cannot listen for peers on port %u: %s\n", peerPort, error.c_str());
        return 1;
    }

    const std::string connectHost = useRelay ? relayHost : host;
    const std::uint16_t connectPort = useRelay ? relayPort : port;
    const std::string advertiseHost = useRelay ? "" : advertise;

    chat::Connection connection;
    if (!connection.connectTo(connectHost, connectPort, 5000, error)) {
        std::fprintf(stderr, "cannot connect to %s:%u: %s; retrying in the background\n",
                     connectHost.c_str(), connectPort, error.c_str());
    } else {
        connection.startReader();
    }

    chat::Tui tui(connection, peers);
    return tui.run(name, connectHost, connectPort, advertiseHost, useRelay);
}
