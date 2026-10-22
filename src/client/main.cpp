#include <cstdio>
#include <string>

#include "client/options.h"
#include "client/tui.h"
#include "common/net.h"
#include "util/log.h"
#if defined(DISQUISITION_COMMUNITIES)
#include "community/terminal.h"
#endif

int main(int argc, char** argv) {
#if defined(DISQUISITION_COMMUNITIES)
    if (argc > 1 && std::string(argv[1]) == "community") return community::runTerminal(argc - 1, argv + 1);
#endif
    const chat::ClientOptions options = chat::parseClientOptions(argc, argv);

    std::string error;
    if (!chat::logToFile("client", options.logFile, error)) {
        std::fprintf(stderr, "cannot open log: %s\n", error.c_str());
        return 1;
    }
    chat::ignoreSigpipe();

    chat::Tui tui;
    return tui.run(options);
}
