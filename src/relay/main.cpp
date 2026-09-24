#include "relay/options.h"
#include "relay/relay.h"

int main(int argc, char** argv) {
    chat::Relay relay(chat::parseRelayOptions(argc, argv));
    return relay.run();
}
