#pragma once

#include <QMap>
#include <QObject>
#include <QPointer>
#include <QTcpServer>
#include <QTimer>

class QTcpSocket;
namespace community {

// A rendezvous and byte tunnel. No community metadata or invite policy lives here.
class Relay final : public QObject {
    Q_OBJECT
public:
    explicit Relay(QObject* parent = nullptr);
    ~Relay() override;
    bool listen(quint16 port, const QHostAddress& address = QHostAddress::Any);
    quint16 port() const { return listener_.serverPort(); }
    QString errorString() const { return listener_.errorString(); }
    void close();

private:
    struct Session;
    void read(Session* session);
    void route(Session* session, const QJsonObject& request);
    void send(Session* session, const QJsonObject& message);
    void bridge(Session* a, Session* b);
    QTcpServer listener_;
    QList<Session*> sessions_;
    QMap<QString, Session*> registered_;
    QMap<QString, Session*> waiting_;
    QTimer timer_;
};
} // namespace community
