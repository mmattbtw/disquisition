#pragma once

#include <QObject>
#include <QPointer>
#include <QSet>
#include <QTimer>
#include <memory>

#include "community/store.h"

class QTcpSocket;
class QTcpServer;
class QSslSocket;

namespace community {

struct Presence {
    QString voiceChannel;
    bool muted = false;
    bool deafened = false;
    bool sharing = false;
};

// One event-loop-owned peer registry across all communities. Relay tunnels
// carry end-to-end TLS; they don't make admission or metadata decisions.
class Node final : public QObject {
    Q_OBJECT
public:
    explicit Node(const QString& directory, QObject* parent = nullptr);
    ~Node() override;
    bool start(const QString& displayName, const QString& advertisedHost = {}, quint16 port = 0,
               const QString& relayEndpoint = {});
    void stop();
    QString peerId() const { return store_.identity().id(); }
    QString endpoint() const { return endpoint_; }
    quint16 port() const;
    const QMap<QString, Model>& communities() const { return models_; }
    QString createCommunity(const QString& name);
    void joinCommunity(const QString& link);
    QString invite(const QString& community);
    void edit(const QString& community, const QString& kind, const QJsonObject& data);
    int sendPacket(const QString& community, const QString& channel, const QString& type,
                   QJsonObject data);
    void setVoice(const QString& community, const QString& channel, bool muted = false,
                  bool deafened = false, bool sharing = false);
    QSet<QString> online(const QString& community) const;
    QString host(const QString& community) const;
    Presence presence(const QString& community, const QString& peer) const;
    qsizetype connectionCount() const;
    qint64 mediaBacklog() const;
    bool synchronizing(const QString& community) const;

signals:
    void changed();
    void joined(const QString& community);
    void joinRejected(const QString& community, const QString& reason);
    void packetReceived(const QString& community, const QString& channel, const QString& peer,
                        const QString& type, const QJsonObject& data);
    void errorOccurred(const QString& message);

private:
    struct Link;
    struct Retry {
        qint64 due = 0;
        int failures = 0;
    };
    void accept(qintptr descriptor);
    Link* makeLink(bool outgoing, const QString& expectedPeer = {});
    void encrypted(Link* link);
    void read(Link* link);
    void handle(Link* link, const QJsonObject& message);
    void send(Link* link, QJsonObject message);
    void sendState(Link* link, const QString& community);
    void offer(Link* link);
    void authorize(Link* link, const QString& community);
    void broadcastState(const QString& community);
    void dial(const QString& peer, const QString& endpoint, const QString& discoverCommunity = {});
    void tick();
    void connectRelay();
    void readRelay();
    void registerRelay();
    void save(Model model);
    void processAdmissions();
    void publishProfile(const QString& community);
    void persistPending();
    bool writable(Link* link, const QString& community) const;
    bool validPacket(const QString& community, const QString& channel, const QString& type,
                     const QJsonObject& data, const QString& peer) const;

    Store store_;
    QMap<QString, Model> models_;
    QMap<QString, Invite> pending_;
    QMap<QString, Link*> joining_;
    QMap<QString, qint64> joinStarted_;
    QMap<QString, QString> rejected_;
    std::unique_ptr<QTcpServer> listener_;
    QList<Link*> links_;
    QMap<QString, Link*> peers_;
    QMap<QString, Retry> retries_;
    QMap<QString, QMap<QString, Presence>> presence_;
    QMap<QString, Presence> mine_;
    QPointer<QTcpSocket> relay_;
    QString relayEndpoint_;
    QString relayNonce_;
    qint64 relayDue_ = 0;
    qint64 relayLastRead_ = 0;
    QString endpoint_;
    QString name_;
    QByteArray certificate_;
    QByteArray privateKey_;
    QTimer timer_;
    bool running_ = false;
    qint64 recoveringUntil_ = 0;
    QSet<QString> seenMessages_;
    QStringList messageOrder_;
};

} // namespace community
