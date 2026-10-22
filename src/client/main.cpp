#include <cstdio>
#include <string>

#include "client/options.h"
#include "client/tui.h"
#include "common/net.h"
#include "util/log.h"

int main(int argc, char** argv) {
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
