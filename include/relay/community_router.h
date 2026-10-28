#pragma once

#include <QMap>
#include <QObject>
#include <QSet>
#include <QTimer>

class QJsonObject;
class QTcpSocket;
namespace chat {

// A rendezvous and byte tunnel. No community metadata or invite policy lives here.
class CommunityRouter final : public QObject {
    Q_OBJECT
public:
    explicit CommunityRouter(QObject* parent = nullptr);
    ~CommunityRouter() override;
    // Takes ownership of a community socket from the shared relay listener.
    void adopt(qintptr descriptor);
    void start();
    void close();

private:
    struct Session;
    void read(Session* session);
    void route(Session* session, const QJsonObject& request);
    void send(Session* session, const QJsonObject& message);
    void bridge(Session* a, Session* b);
    QList<Session*> sessions_;
    QMap<QString, Session*> registered_;
    QMap<QString, Session*> waiting_;
    QTimer timer_;
};
} // namespace chat
