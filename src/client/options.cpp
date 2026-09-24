#include "client/options.h"

#include <cstdio>
#include <cstdlib>
#include <iostream>

#include "common/command_line.h"
#include "common/net.h"
#include "common/protocol.h"

namespace chat {
namespace {

constexpr const char* kUsage = R"(usage: client [options]

  -H, --host <host>    server host (default 127.0.0.1)
  -p, --port <port>    server port (default 9000)
  -n, --name <name>    sign in automatically with this name
      --p2p-port <port>  listen port for direct peer connections
                       (default 0, which picks a free port)
      --advertise <host> address to tell peers to dial for p2p, instead of
                       the address the server sees you connect from. Use
                       your public IP or hostname to accept peers over the
                       internet (pair with --p2p-port + a port forward)
      --local          automatically advertise your local IPv4 address
      --relay <host[:port]>  reach the mesh through a relay instead of
                       accepting direct connections. Your traffic and
                       everyone else's is tunnelled through it, so no
                       port forward is needed (default port 3333)
      --leak-my-ip     if the relay drops, fall back to a direct connection
                       while continuing to retry the relay
      --log <file>     write a debug log to this file
  -h, --help           show this message
)";

constexpr const char* kPublicServer = "relay.mmatt.net";
constexpr std::uint16_t kDefaultServerPort = 9000;
constexpr std::uint16_t kDefaultRelayPort = 3333;

bool readLine(const char* prompt, std::string& answer) {
    std::cout << prompt << std::flush;
    if (!std::getline(std::cin, answer)) {
        return false;
    }
    answer = trim(answer);
    return true;
}

bool promptForOptions(ClientOptions& options) {
    options.host = kPublicServer;
    options.port = kDefaultServerPort;

    std::string answer;
    if (!readLine("server host [relay.mmatt.net]: ", answer)) {
        return false;
    }
    if (!answer.empty()) {
        options.host = answer;
    }

    for (;;) {
        if (!readLine("server port [9000]: ", answer)) {
            return false;
        }
        if (answer.empty() || parsePort(answer, options.port, false)) {
            break;
        }
        std::cout << "please enter a port from 1 to 65535\n";
    }

    for (;;) {
        if (!readLine("name: ", answer)) {
            return false;
        }
        options.name = sanitizeName(answer);
        if (!options.name.empty()) {
            return true;
        }
        std::cout << "please enter a name\n";
    }
}

// Settles the options that depend on each other once all flags are read.
void resolveRelay(CommandLine& args, ClientOptions& options, const std::string& relay,
                  bool hostGiven) {
    options.useRelay = !relay.empty();
    if (!options.useRelay) {
        if (options.leakMyIp) {
            args.fail("--leak-my-ip requires --relay");
        }
        return;
    }
    if (!parseHostPort(relay, options.relayHost, options.relayPort, kDefaultRelayPort)) {
        std::fprintf(stderr, "invalid --relay value: expected host or host:port\n");
        std::exit(1);
    }
    if (options.leakMyIp) {
        // The relay usually runs beside the server, so fall back to its host.
        if (!hostGiven) {
            options.host = options.relayHost;
        }
    }
    else {
        // The relay is this client's only public address.
        options.advertiseHost.clear();
    }
}

} // namespace

ClientOptions parseClientOptions(int argc, char** argv) {
    ClientOptions options;
    if (argc == 1) {
        if (!promptForOptions(options)) {
            std::fprintf(stderr, "setup cancelled before connecting\n");
            std::exit(1);
        }
        return options;
    }

    std::string relay;
    bool hostGiven = false;
    bool localGiven = false;
    bool advertiseGiven = false;
    CommandLine args(argc, argv, kUsage);
    while (args.next()) {
        if (args.is("-H", "--host")) {
            options.host = args.value();
            hostGiven = true;
        }
        else if (args.is("-p", "--port")) {
            options.port = args.port();
        }
        else if (args.is("-n", "--name")) {
            options.name = args.value();
        }
        else if (args.is("--p2p-port")) {
            options.peerPort = args.port(/*allowZero=*/true);
        }
        else if (args.is("--advertise")) {
            options.advertiseHost = args.value();
            advertiseGiven = true;
        }
        else if (args.is("--local")) {
            localGiven = true;
        }
        else if (args.is("--relay")) {
            relay = args.value();
        }
        else if (args.is("--leak-my-ip")) {
            options.leakMyIp = true;
        }
        else if (args.is("--log")) {
            options.logFile = args.value();
        }
        else if (args.is("-h", "--help")) {
            args.showHelp();
        }
        else {
            args.rejectOption();
        }
    }
    if (localGiven && advertiseGiven) {
        args.fail("--local and --advertise cannot be used together");
    }
    resolveRelay(args, options, relay, hostGiven);
    if (localGiven && (!options.useRelay || options.leakMyIp)) {
        options.advertiseHost = localIpAddress(options.host, options.port);
        if (options.advertiseHost.empty()) {
            args.fail("--local could not find an active non-loopback IPv4 address");
        }
    }
    return options;
}

} // namespace chat
