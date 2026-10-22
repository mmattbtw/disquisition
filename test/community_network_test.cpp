#include <QCoreApplication>
#include <QDeadlineTimer>
#include <QProcess>
#include <QRegularExpression>
#include <QSslSocket>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QThread>
#include <cstdio>
#include <functional>

#include "check.h"
#include "common/protocol.h"
#include "community/node.h"
#include "community/wire.h"
#include "relay/community_router.h"

using namespace community;
namespace {
bool until(const std::function<bool()>& predicate, int timeout = 12000) {
    QDeadlineTimer deadline(timeout);
    while (!deadline.hasExpired()) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        if (predicate())
            return true;
        QThread::msleep(1);
    }
    return predicate();
}
QProcess* relayProcess = nullptr;
QProcess* serverProcess = nullptr;
QString relayExecutable, serverExecutable;
void stopProcesses() {
    for (auto* process : {relayProcess, serverProcess}) {
        if (process && process->state() != QProcess::NotRunning) {
            process->kill();
            process->waitForFinished();
        }
    }
}

// Desktop-only builds exercise the routing component in process. POSIX builds
// run the existing relay executable, including its shared-port dispatcher.
class TestRelay : public QTcpServer {
public:
    TestRelay() {
        if (relayExecutable.isEmpty()) {
            router.start();
            CHECK(listen(QHostAddress::LocalHost, 0));
            port_ = serverPort();
        }
        else {
            relayProcess = &relay;
            serverProcess = &server;
            gCheckCleanup = stopProcesses;
            const auto serverPort = start(server, serverExecutable,
                                          {"--port", "0", "--db", ":memory:"});
            port_ = start(relay, relayExecutable,
                          {"--host", "127.0.0.1", "--port", QString::number(serverPort),
                           "--advertise", "127.0.0.1", "--listen", "0"});
        }
    }
    ~TestRelay() {
        if (relay.state() != QProcess::NotRunning) {
            relay.terminate();
            CHECK(relay.waitForFinished(8000));
            CHECK(relay.exitStatus() == QProcess::NormalExit && relay.exitCode() == 0);
            const auto output = relay.readAll();
            CHECK(!output.contains("QSocketNotifier") && !output.contains("QObject::"));
        }
        stopProcesses();
        relayProcess = serverProcess = nullptr;
        gCheckCleanup = nullptr;
    }
    quint16 port() const { return port_; }
    void stopDiscovery() {
        server.kill();
        CHECK(server.waitForFinished());
    }

protected:
    void incomingConnection(qintptr descriptor) override { router.adopt(descriptor); }

private:
    quint16 start(QProcess& process, const QString& executable, const QStringList& args) {
        process.setProcessChannelMode(QProcess::MergedChannels);
        process.start(executable, args);
        CHECK(process.waitForStarted());
        QByteArray output;
        QRegularExpression marker("listening on port ([0-9]+)");
        CHECK(until([&] {
            output += process.readAll();
            return marker.match(QString::fromUtf8(output)).hasMatch();
        }));
        return marker.match(QString::fromUtf8(output)).captured(1).toUShort();
    }
    chat::CommunityRouter router;
    QProcess server, relay;
    quint16 port_ = 0;
};

struct RoomPeer {
    QTcpSocket socket;
    std::string buffer;
    bool received(chat::MsgType type, const std::string& body = {}) {
        const auto bytes = socket.readAll();
        buffer.append(bytes.constData(), bytes.size());
        chat::Message message;
        while (chat::decode(buffer, message) == chat::DecodeStatus::Ok) {
            if (message.type == type &&
                (body.empty() || (message.fields.size() >= 3 && message.fields[2] == body)))
                return true;
        }
        return false;
    }
    void send(const chat::Message& message) {
        const auto bytes = chat::encode(message);
        socket.write(bytes.data(), bytes.size());
    }
    void login(quint16 port, const std::string& name) {
        socket.connectToHost("127.0.0.1", port);
        CHECK(socket.waitForConnected());
        send({chat::MsgType::Login, {name}});
        CHECK(until([&] { return received(chat::MsgType::LoginOk); }));
    }
};

void diagnostics(Node& node) {
    QObject::connect(&node, &Node::errorOccurred, [](const QString& message) {
        std::fprintf(stderr, "node: %s\n", message.toUtf8().constData());
    });
}

void directListenerReportsRelayAddressMixupsWithoutTlsErrors() {
    QTemporaryDir desktopData, clientData;
    Node desktop(desktopData.path()), client(clientData.path());
    QStringList desktopErrors, clientErrors;
    QObject::connect(&desktop, &Node::errorOccurred,
                     [&](const QString& message) { desktopErrors.append(message); });
    QObject::connect(&client, &Node::errorOccurred,
                     [&](const QString& message) { clientErrors.append(message); });
    CHECK(desktop.start("desktop", "127.0.0.1"));
    CHECK(client.start("misconfigured", {}, 0, "127.0.0.1:" + QString::number(desktop.port())));
    CHECK(until([&] { return !clientErrors.isEmpty(); }));
    CHECK(clientErrors.size() == 1);
    CHECK(clientErrors[0].contains("desktop client's direct address"));
    CHECK(desktopErrors.isEmpty());
    CHECK(client.communities().isEmpty());
}

void relayDiagnosticsAreReportedOnceAcrossRetries() {
    // Exercise both an explicit protocol rejection and an older relay that
    // simply closes an unsupported handshake. Neither should spam the status.
    for (const bool diagnostic : {true, false}) {
        QTcpServer unavailable;
        CHECK(unavailable.listen(QHostAddress::LocalHost, 0));
        int attempts = 0;
        QObject::connect(&unavailable, &QTcpServer::newConnection, [&] {
            while (unavailable.hasPendingConnections()) {
                auto* socket = unavailable.nextPendingConnection();
                ++attempts;
                // Read the prefix before closing, just as a protocol dispatcher
                // does. This also avoids resetting a socket with unread data.
                QObject::connect(socket, &QTcpSocket::readyRead, socket, [socket, diagnostic] {
                    socket->readAll();
                    if (diagnostic)
                        socket->write(
                            frame({{"type", "error"}, {"message", "This relay needs an update."}}));
                    socket->disconnectFromHost();
                });
                QObject::connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
            }
        });
        QTemporaryDir directory;
        Node client(directory.path());
        QStringList errors;
        QObject::connect(&client, &Node::errorOccurred,
                         [&](const QString& message) { errors.append(message); });
        CHECK(
            client.start("alice", {}, 0, "127.0.0.1:" + QString::number(unavailable.serverPort())));
        // A third attempt proves the client processed two failures, including
        // the retry whose duplicate diagnostic must have been suppressed.
        CHECK(until([&] { return attempts >= 3; }));
        CHECK(errors.size() == 1);
        CHECK(diagnostic ? errors[0] == "This relay needs an update."
                         : errors[0].contains("closed the community handshake"));
    }
}

QString channel(const Model& model, const QString& type) {
    for (const auto& item : model.channels)
        if (item.type == type)
            return item.id;
    return {};
}

void directAdmissionIsolationAndConnectionReuse() {
    QTemporaryDir ad, bd, cd, dd;
    Node a(ad.path()), b(bd.path()), c(cd.path()), denied(dd.path());
    diagnostics(a);
    diagnostics(b);
    diagnostics(c);
    diagnostics(denied);
    CHECK(a.start("alice", "127.0.0.1") && b.start("bob", "127.0.0.1") &&
          c.start("carol", "127.0.0.1") && denied.start("denied", "127.0.0.1"));
    const auto cid = a.createCommunity("one");
    const auto link = a.invite(cid);
    b.joinCommunity(link);
    CHECK(until([&] { return a.online(cid).size() == 2 && b.online(cid).size() == 2; }));
    CHECK(a.connectionCount() == 1 && b.connectionCount() == 1);
    const auto second = a.createCommunity("two");
    b.joinCommunity(a.invite(second));
    CHECK(until([&] { return b.online(second).size() == 2 && a.online(second).size() == 2; }));
    CHECK(a.connectionCount() == 1 && b.connectionCount() == 1);
    bool contactedBackup = false;
    QObject::connect(&denied, &Node::changed, [&] {
        if (denied.connectionCount() > 0)
            contactedBackup = true;
    });
    auto withBackup = Invite::fromLink(link);
    withBackup.seeds.append(
        QJsonObject{{"peer", denied.peerId()}, {"endpoint", denied.endpoint()}});
    c.joinCommunity(withBackup.toLink());
    CHECK(until([&] {
        return a.online(cid).size() == 3 && b.online(cid).size() == 3 && c.online(cid).size() == 3;
    }));
    CHECK(!contactedBackup); // A reachable first seed must not race stale backups.
    int bobMessages = 0, carolMessages = 0;
    QObject::connect(&b, &Node::packetReceived,
                     [&](const QString& community, const QString&, const QString&,
                         const QString& kind, const QJsonObject&) {
                         if (community == second && kind == "chat")
                             ++bobMessages;
                     });
    QObject::connect(&c, &Node::packetReceived,
                     [&](const QString&, const QString&, const QString&, const QString& kind,
                         const QJsonObject&) {
                         if (kind == "chat")
                             ++carolMessages;
                     });
    CHECK(a.sendPacket(second, channel(a.communities()[second], "text"), "chat",
                       {{"body", "private to community two"}}) == 1);
    CHECK(until([&] { return bobMessages == 1; }));
    CHECK(carolMessages == 0);
    auto bad = Invite::fromLink(link);
    bad.code = randomCode();
    bool rejected = false;
    QObject::connect(&denied, &Node::joinRejected,
                     [&](const QString&, const QString&) { rejected = true; });
    denied.joinCommunity(bad.toLink());
    CHECK(until([&] { return rejected; }));
    CHECK(denied.communities().isEmpty());

    const auto voice = channel(a.communities()[cid], "voice");
    const auto otherVoice = freshId();
    a.edit(cid, "channel",
           {{"id", otherVoice}, {"name", "other"}, {"type", "voice"}, {"topic", ""}});
    CHECK(until([&] {
        return c.communities()[cid].channels.contains(otherVoice) && !a.synchronizing(cid) &&
               !b.synchronizing(cid);
    }));
    a.setVoice(cid, voice);
    b.setVoice(cid, voice);
    c.setVoice(cid, otherVoice);
    CHECK(until([&] {
        return a.presence(cid, b.peerId()).voiceChannel == voice &&
               b.presence(cid, a.peerId()).voiceChannel == voice &&
               a.presence(cid, c.peerId()).voiceChannel == otherVoice;
    }));
    int bobAudio = 0, carolAudio = 0;
    QObject::connect(&b, &Node::packetReceived,
                     [&](const QString&, const QString&, const QString&, const QString& kind,
                         const QJsonObject&) {
                         if (kind == "audio")
                             ++bobAudio;
                     });
    QObject::connect(&c, &Node::packetReceived,
                     [&](const QString&, const QString&, const QString&, const QString& kind,
                         const QJsonObject&) {
                         if (kind == "audio")
                             ++carolAudio;
                     });
    CHECK(a.sendPacket(cid, voice, "audio",
                       {{"bytes", QString::fromLatin1(QByteArray(640, '\0').toBase64())}}) == 1);
    CHECK(until([&] { return bobAudio == 1; }));
    CHECK(carolAudio == 0);
}

void relayHostMigrationAndReconnection() {
    TestRelay relay;
    // A stalled or fragmented initial handshake must not block other clients.
    QTcpSocket stalled, fragmented;
    stalled.connectToHost("127.0.0.1", relay.port());
    CHECK(stalled.waitForConnected());
    fragmented.connectToHost("127.0.0.1", relay.port());
    CHECK(fragmented.waitForConnected());
    fragmented.write("DQ", 2);
    CHECK(!until([&] { return fragmented.bytesAvailable() > 0; }, 100));
    fragmented.write("C1\n", 3);
    CHECK(until([&] { return fragmented.bytesAvailable() > 0; }));
    QByteArray challengeBytes;
    QJsonObject challenge;
    CHECK(until([&] {
        challengeBytes += fragmented.readAll();
        return takeFrame(challengeBytes, challenge);
    }));
    CHECK(challenge.value("type") == "challenge");
    fragmented.abort();
    RoomPeer roomA, roomB;
    if (!relayExecutable.isEmpty()) {
        roomA.login(relay.port(), "room-a");
        roomB.login(relay.port(), "room-b");
        CHECK(until([&] {
            roomA.send(
                {chat::MsgType::PeerChat, {"room-a", "12345", "before communities", "mint"}});
            return roomB.received(chat::MsgType::PeerChat, "before communities");
        }));
    }
    const auto endpoint = "relay://127.0.0.1:" + QString::number(relay.port());
    QTemporaryDir ad, bd, cd;
    Node a(ad.path()), b(bd.path()), c(cd.path());
    diagnostics(a);
    diagnostics(b);
    diagnostics(c);
    CHECK(a.start("alice", {}, 0, endpoint) && b.start("bob", {}, 0, endpoint) &&
          c.start("carol", {}, 0, endpoint));
    CHECK(a.port() == 0 && b.port() == 0); // relay mode never opens a direct listener
    if (!relayExecutable.isEmpty()) {
        roomA.send({chat::MsgType::PeerChat, {"room-a", "12346", "shared relay", "mint"}});
        CHECK(until([&] { return roomB.received(chat::MsgType::PeerChat, "shared relay"); }));
        // Communities continue to join and migrate hosts without discovery.
        relay.stopDiscovery();
    }
    const auto cid = a.createCommunity("relayed");
    const auto link = a.invite(cid);
    b.joinCommunity(link);
    CHECK(until([&] { return a.online(cid).size() == 2 && b.online(cid).size() == 2; }));
    bool chatReceived = false;
    QObject::connect(&b, &Node::packetReceived,
                     [&](const QString& community, const QString&, const QString&,
                         const QString& kind, const QJsonObject& packet) {
                         if (community == cid && kind == "chat" &&
                             packet.value("body") == "community through shared relay")
                             chatReceived = true;
                     });
    CHECK(a.sendPacket(cid, channel(a.communities()[cid], "text"), "chat",
                       {{"body", "community through shared relay"}}) == 1);
    CHECK(until([&] { return chatReceived; }));
    if (!relayExecutable.isEmpty()) {
        roomA.send({chat::MsgType::PeerChat, {"room-a", "12347", "room beside community", "mint"}});
        CHECK(until(
            [&] { return roomB.received(chat::MsgType::PeerChat, "room beside community"); }));
    }
    a.stop();
    CHECK(until([&] { return b.online(cid).size() == 1; }));
    CHECK(b.host(cid) == b.peerId());
    // The original invite names Alice. The relay discovers Bob after she
    // disconnects; Bob validates the existing invite without any admin online.
    c.joinCommunity(link);
    CHECK(until([&] { return b.online(cid).size() == 2 && c.online(cid).size() == 2; }));
    bool denied = false;
    try {
        b.edit(cid, "rename", {{"name", "not an admin"}});
    }
    catch (const std::exception&) {
        denied = true;
    }
    CHECK(denied);
    CHECK(a.start("alice", {}, 0, endpoint));
    CHECK(until([&] {
        return a.online(cid).size() == 3 && a.communities()[cid].members.size() == 3 &&
               !a.synchronizing(cid) && !b.synchronizing(cid);
    }));
    a.edit(cid, "admin", {{"peer", b.peerId()}, {"enabled", true}});
    CHECK(
        until([&] { return b.communities()[cid].canAdmin(b.peerId()) && !b.synchronizing(cid); }));
    a.stop();
    CHECK(until([&] { return b.online(cid).size() == 2; }));
    b.edit(cid, "rename", {{"name", "admin acting alone"}});
    CHECK(until([&] { return c.communities()[cid].name == "admin acting alone"; }));
    CHECK(a.start("alice", {}, 0, endpoint));
    CHECK(until([&] {
        return a.communities()[cid].name == "admin acting alone" && a.online(cid).size() == 3;
    }));
}

void returningHostSynchronizesBeforeAcceptingInvites() {
    QTemporaryDir ad, bd, dd;
    Node a(ad.path()), b(bd.path()), denied(dd.path());
    diagnostics(a);
    diagnostics(b);
    diagnostics(denied);
    CHECK(a.start("alice", "127.0.0.1") && b.start("bob", "127.0.0.1") &&
          denied.start("new peer", "127.0.0.1"));
    const auto cid = a.createCommunity("revocation sync");
    const auto link = a.invite(cid);
    b.joinCommunity(link);
    CHECK(until([&] {
        return a.online(cid).size() == 2 && b.online(cid).size() == 2 && !a.synchronizing(cid);
    }));
    const auto bobEndpoint = b.endpoint();
    b.stop();
    CHECK(until([&] { return a.online(cid).size() == 1; }));
    const auto decoded = Invite::fromLink(link);
    a.edit(cid, "revoke", {{"hash", digest(decoded.code.toUtf8())}});
    CHECK(b.communities()[cid].validCode(decoded.code));
    CHECK(b.start("bob", "127.0.0.1"));
    auto throughBob = decoded;
    throughBob.seeds = {QJsonObject{{"peer", b.peerId()}, {"endpoint", bobEndpoint}}};
    bool rejected = false;
    QObject::connect(&denied, &Node::joinRejected,
                     [&](const QString&, const QString&) { rejected = true; });
    denied.joinCommunity(throughBob.toLink());
    CHECK(until([&] { return rejected; }));
    CHECK(denied.communities().isEmpty());
    CHECK(!b.communities()[cid].validCode(decoded.code));
}

void preferredHostCoordinatesAdmissions() {
    QTemporaryDir ad, bd, cd;
    Node a(ad.path()), b(bd.path()), c(cd.path());
    diagnostics(a);
    diagnostics(b);
    diagnostics(c);
    CHECK(a.start("alice", "127.0.0.1") && b.start("bob", "127.0.0.1") &&
          c.start("carol", "127.0.0.1"));
    const auto cid = a.createCommunity("host priority");
    b.joinCommunity(a.invite(cid));
    CHECK(until([&] {
        return a.online(cid).size() == 2 && b.online(cid).size() == 2 && !a.synchronizing(cid);
    }));
    a.edit(cid, "priority", {{"peers", QJsonArray{b.peerId(), a.peerId()}}});
    CHECK(until([&] {
        return a.host(cid) == b.peerId() && b.host(cid) == b.peerId() && !a.synchronizing(cid);
    }));
    auto invite = Invite::fromLink(a.invite(cid));
    CHECK(until([&] { return a.host(cid) == b.peerId() && !a.synchronizing(cid); }));
    // A stale/bootstrap address still reaches the preferred host by redirect.
    invite.seeds = {QJsonObject{{"peer", a.peerId()}, {"endpoint", a.endpoint()}}};
    c.joinCommunity(invite.toLink());
    CHECK(until([&] {
        return a.online(cid).size() == 3 && b.online(cid).size() == 3 && c.online(cid).size() == 3;
    }));
    bool coordinated = false;
    for (const auto& value : a.communities()[cid].toJson().value("operations").toArray()) {
        const auto operation = value.toObject();
        if (operation.value("kind") == "admit" && operation.value("data")
                                                          .toObject()
                                                          .value("intent")
                                                          .toObject()
                                                          .value("peer")
                                                          .toString() == c.peerId())
            coordinated = operation.value("author").toString() == b.peerId();
    }
    CHECK(coordinated);
}

void allOfflineRestoresMetadataAndNeverReplaysMissedChat() {
    QTemporaryDir ad, bd;
    QString cid;
    quint16 alicePort = 0, bobPort = 0;
    {
        Node a(ad.path()), b(bd.path());
        diagnostics(a);
        diagnostics(b);
        CHECK(a.start("alice", "127.0.0.1") && b.start("bob", "127.0.0.1"));
        cid = a.createCommunity("persistent without chat saving");
        b.joinCommunity(a.invite(cid));
        CHECK(until([&] { return a.online(cid).size() == 2 && b.online(cid).size() == 2; }));
        alicePort = a.port();
        bobPort = b.port();
    }
    {
        Node a(ad.path()), b(bd.path());
        diagnostics(a);
        diagnostics(b);
        CHECK(a.communities()[cid].members.size() == 2 && b.communities()[cid].members.size() == 2);
        int messages = 0;
        QObject::connect(&b, &Node::packetReceived,
                         [&](const QString&, const QString&, const QString&, const QString& kind,
                             const QJsonObject&) {
                             if (kind == "chat")
                                 ++messages;
                         });
        CHECK(a.start("alice", "127.0.0.1"));
        const auto text = channel(a.communities()[cid], "text");
        CHECK(a.sendPacket(cid, text, "chat", {{"body", "sent while alone"}}) == 0);
        CHECK(b.start("bob", "127.0.0.1"));
        CHECK(a.port() == alicePort && b.port() == bobPort);
        CHECK(until([&] { return a.online(cid).size() == 2 && b.online(cid).size() == 2; }));
        CHECK(messages == 0);
        CHECK(a.sendPacket(cid, text, "chat", {{"body", "live after reconnect"}}) == 1);
        CHECK(until([&] { return messages == 1; }));
    }
}
} // namespace
int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    if (argc == 3) {
        relayExecutable = QString::fromLocal8Bit(argv[1]);
        serverExecutable = QString::fromLocal8Bit(argv[2]);
    }
    const auto backend = qEnvironmentVariable("DISQUISITION_TEST_TLS_BACKEND");
    if (!backend.isEmpty() && !QSslSocket::setActiveBackend(backend)) {
        std::fprintf(stderr, "TLS backend unavailable: %s\n", backend.toUtf8().constData());
        return 77;
    }
    directAdmissionIsolationAndConnectionReuse();
    directListenerReportsRelayAddressMixupsWithoutTlsErrors();
    relayDiagnosticsAreReportedOnceAcrossRetries();
    relayHostMigrationAndReconnection();
    returningHostSynchronizesBeforeAcceptingInvites();
    preferredHostCoordinatesAdmissions();
    allOfflineRestoresMetadataAndNeverReplaysMissedChat();
    std::puts("community_network_test: ok");
}
