#include "relay/options.h"

#include "common/command_line.h"

namespace chat {
namespace {

constexpr const char* kUsage = R"(usage: relay --advertise <host> [options]

  -H, --host <host>       chat server host (default 127.0.0.1)
  -p, --port <port>       chat server port (default 9000)
      --advertise <host>  public host other peers should dial to reach
                          this relay (e.g. relay.example.net). Strongly
                          recommended: it is what lets unreachable
                          clients be discovered and dialled
      --listen <port>     the one port every hosted user shares, for
                          clients and peers alike (default 3333)
  -h, --help              show this message

Names come from the clients: any number of clients may attach, each
signing in as the name it supplies. Point a client at it with:
  client --relay <advertise-host>:<listen>
)";

} // namespace

RelayOptions parseRelayOptions(int argc, char** argv) {
    RelayOptions options;
    CommandLine args(argc, argv, kUsage);
    while (args.next()) {
        if (args.is("-H", "--host")) {
            options.serverHost = args.value();
        }
        else if (args.is("-p", "--port")) {
            options.serverPort = args.port();
        }
        else if (args.is("--advertise")) {
            options.advertiseHost = args.value();
        }
        else if (args.is("--listen")) {
            options.port = args.port(/*allowZero=*/true);
        }
        else if (args.is("-h", "--help")) {
            args.showHelp();
        }
        else {
            args.rejectOption();
        }
    }
    return options;
}

} // namespace chat
