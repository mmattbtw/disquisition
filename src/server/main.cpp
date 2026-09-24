#include "server/options.h"
#include "server/server.h"

int main(int argc, char** argv) {
    return chat::runServer(chat::parseServerOptions(argc, argv));
}
