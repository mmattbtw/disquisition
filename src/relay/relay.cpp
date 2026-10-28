#include "relay/relay.h"

namespace chat {

Relay::Relay(QObject* parent) : QTcpServer(parent), router_(this) {
    router_.start();
}

void Relay::incomingConnection(qintptr descriptor) {
    router_.adopt(descriptor);
}

void Relay::close() {
    QTcpServer::close();
    router_.close();
}

} // namespace chat
