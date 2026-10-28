#pragma once

#include <QTcpServer>

#include "relay/community_router.h"

namespace chat {

// The community relay's only listener. All peer traffic remains inside
// end-to-end TLS tunnels; metadata and admission decisions stay on clients.
class Relay final : public QTcpServer {
public:
    explicit Relay(QObject* parent = nullptr);
    void close();

protected:
    void incomingConnection(qintptr descriptor) override;

private:
    CommunityRouter router_;
};

} // namespace chat
