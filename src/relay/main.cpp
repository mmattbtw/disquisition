#include "relay/options.h"
#include "relay/relay.h"

#if defined(DISQUISITION_COMMUNITIES)
#    include <QCoreApplication>
#endif

int main(int argc, char** argv) {
#if defined(DISQUISITION_COMMUNITIES)
    QCoreApplication app(argc, argv);
#endif
    chat::Relay relay(chat::parseRelayOptions(argc, argv));
    return relay.run();
}
