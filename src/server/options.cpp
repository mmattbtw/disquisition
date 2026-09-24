#include "server/options.h"

#include "common/command_line.h"
#include "common/protocol.h"

namespace chat {
namespace {

constexpr const char* kUsage = R"(usage: server [options]

  -p, --port <port>        port to listen on (default 9000)
  -d, --db <path>          sqlite database file (default chat.db)
      --history <count>    messages replayed to new clients (default 50)
  -h, --help               show this message
)";

} // namespace

ServerOptions parseServerOptions(int argc, char** argv) {
    ServerOptions options;
    CommandLine args(argc, argv, kUsage);
    while (args.next()) {
        if (args.is("-p", "--port")) {
            options.port = args.port(/*allowZero=*/true);
        }
        else if (args.is("-d", "--db")) {
            options.databasePath = args.value();
        }
        else if (args.is("--history")) {
            std::int64_t limit = 0;
            if (!parseInt64(args.value(), limit) || limit < 0) {
                args.fail("history count out of range");
            }
            options.historyLimit = static_cast<std::size_t>(limit);
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
