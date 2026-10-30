#include <QCoreApplication>
#include <QDeadlineTimer>
#include <QProcess>
#include <QRegularExpression>
#include <QSslSocket>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QThread>
#include <QThreadPool>
#include <QSemaphore>
#include <cstdio>
#include <functional>

#include "check.h"
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
QString relayExecutable;
void stopProcesses() {
    if (relayProcess && relayProcess->state() != QProcess::NotRunning) {
        relayProcess->kill();
        relayProcess->waitForFinished();
    }
}

// Desktop-only builds exercise the routing component in process. When built,
// run the actual relay executable with no other service present.
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
            gCheckCleanup = stopProcesses;
            port_ = start(relay, relayExecutable, {"--listen", "0"});
        }
    }
    ~TestRelay() {
        if (relay.state() != QProcess::NotRunning) {
#if defined(Q_OS_WIN)
            // QProcess::terminate sends WM_CLOSE, which console apps do not handle.
            relay.kill();
            CHECK(relay.waitForFinished(8000));
#else
            relay.terminate();
            CHECK(relay.waitForFinished(8000));
            CHECK(relay.exitStatus() == QProcess::NormalExit && relay.exitCode() == 0);
#endif
            const auto output = relay.readAll();
            CHECK(!output.contains("QSocketNotifier") && !output.contains("QObject::"));
        }
        stopProcesses();
        relayProcess = nullptr;
        gCheckCleanup = nullptr;
    }
    quint16 port() const { return port_; }
    void interrupt() {
        if (relayExecutable.isEmpty()) {
            close();
            router.close();
        }
        else {
            relay.kill();
            CHECK(relay.waitForFinished(8000));
        }
    }
    void resume() {
        if (relayExecutable.isEmpty()) {
            router.start();
            CHECK(listen(QHostAddress::LocalHost, port_));
        }
        else
            CHECK(start(relay, relayExecutable, {"--listen", QString::number(port_)}) == port_);
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
    QProcess relay;
    quint16 port_ = 0;
};

// Delay metadata work without relying on machine speed or a huge fixture.
// The main event loop must still service timers and allow local edits.
void metadataValidationYieldsToTheEventLoop() {
    QTemporaryDir ad, bd;
    Node a(ad.path()), b(bd.path());
    CHECK(a.start("alice", "127.0.0.1"));
    CHECK(b.start("bob", "127.0.0.1"));
    const auto cid = a.createCommunity("before sync");
    const auto invitation = a.invite(cid);
    auto* pool = QThreadPool::globalInstance();
    const int threads = pool->maxThreadCount();
    pool->setMaxThreadCount(1);
    QSemaphore started, release;
    pool->start([&] { started.release(); release.acquire(); });
    CHECK(started.tryAcquire(1, 5000));
    b.joinCommunity(invitation);
    CHECK(until([&] { return a.communities()[cid].isMember(b.peerId()); }));
    bool timerRan = false;
    QTimer::singleShot(200, &a, [&] { timerRan = true; });
    CHECK(until([&] { return timerRan; }));
    CHECK(!b.communities().contains(cid));
    a.edit(cid, "rename", {{"name", "edited during sync"}});
    release.release();
    CHECK(until([&] {
        return b.communities().contains(cid) && b.communities()[cid].name == "edited during sync" &&
               a.communities()[cid].name == "edited during sync" &&
               !a.synchronizing(cid) && !b.synchronizing(cid);
    }));
    // An established admin edits its own model while an incoming merge is queued.
    a.edit(cid, "admin", {{"peer", b.peerId()}, {"enabled", true}});
    CHECK(until([&] { return b.communities()[cid].canAdmin(b.peerId()) &&
                             !a.synchronizing(cid) && !b.synchronizing(cid); }));
    pool->start([&] { started.release(); release.acquire(); });
    CHECK(started.tryAcquire(1, 5000));
    bool disconnectedDuringValidation = false;
    const auto connection = QObject::connect(&b, &Node::changed, [&] {
        if (b.connectionCount() == 0)
            disconnectedDuringValidation = true;
    });
    a.edit(cid, "rename", {{"name", "remote edit"}});
    timerRan = false;
    // Completed transfers must not expire at the 20-second receive deadline
    // while local validation is still queued.
    QTimer::singleShot(21000, &a, [&] { timerRan = true; });
    CHECK(until([&] { return timerRan; }, 23000));
    CHECK(!disconnectedDuringValidation && b.connectionCount() == 1);
    QObject::disconnect(connection);
    CHECK(b.communities()[cid].name == "edited during sync");
    QString text;
    for (const auto& channel : b.communities()[cid].channels)
        if (channel.type == "text")
            text = channel.id;
    b.edit(cid, "channel", {{"id", text}, {"name", "general"}, {"type", "text"},
                            {"topic", "local edit survives"}});
    release.release();
    CHECK(until([&] {
        return b.communities()[cid].name == "remote edit" &&
               b.communities()[cid].channels[text].topic == "local edit survives" &&
               a.communities()[cid].channels[text].topic == "local edit survives" &&
               !a.synchronizing(cid) && !b.synchronizing(cid);
    }));
    // Stopping a peer must invalidate any queued completion without blocking shutdown.
    pool->start([&] { started.release(); release.acquire(); });
    CHECK(started.tryAcquire(1, 5000));
    a.edit(cid, "rename", {{"name", "stopped during sync"}});
    timerRan = false;
    QTimer::singleShot(200, &a, [&] { timerRan = true; });
    CHECK(until([&] { return timerRan; }));
    CHECK(b.communities()[cid].name == "remote edit");
    b.stop();
    release.release();
    pool->waitForDone();
    pool->setMaxThreadCount(threads);
    CHECK(b.start("bob", "127.0.0.1"));
    CHECK(until([&] { return b.communities()[cid].name == "stopped during sync" &&
                             !b.synchronizing(cid); }));
}

void relayRejectsInvalidAndRetiredOptions() {
    if (relayExecutable.isEmpty())
        return;
    const QList<QStringList> invalid{
        {"--host", "127.0.0.1"}, {"--port", "9000"},     {"--advertise", "relay.example"},
        {"--listen", "65536"},   {"--listen", "-1"},     {"--listen", "abc"},
        {"--listen", "+3333"},   {"--listen", " 3333 "}, {"--listen"}};
    for (const auto& args : invalid) {
        QProcess process;
        process.start(relayExecutable, args);
        CHECK(process.waitForStarted());
        const bool finished = process.waitForFinished(3000);
        if (!finished) {
            process.kill();
            process.waitForFinished();
        }
        CHECK(finished && process.exitStatus() == QProcess::NormalExit && process.exitCode() != 0);
    }
}

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

void largeScreenFrameDelivers(Node& a, Node& b, const QString& cid, const QString& voice) {
    a.setVoice(cid, voice, false, false, true);
    b.setVoice(cid, voice);
    CHECK(until([&] {
        return b.presence(cid, a.peerId()).sharing &&
               a.presence(cid, b.peerId()).voiceChannel == voice;
    }));
    int videoChunks = 0;
    QByteArray video;
    const auto receive = QObject::connect(&b, &Node::packetReceived,
                                          [&](const QString&, const QString&, const QString&,
                                              const QString& kind, const QJsonObject& data) {
                                              if (kind == "screen") {
                                                  ++videoChunks;
                                                  video += QByteArray::fromBase64(
                                                      data.value("bytes").toString().toLatin1());
                                              }
                                          });
    const QByteArray chunk(16 * 1024 - 256, 'v');
    CHECK(a.sendScreenFrame(cid, voice, b.peerId(), 7, QList<QByteArray>(8, chunk), true));
    // A subsequent frame is skipped whole while the first remains queued.
    CHECK(!a.sendScreenFrame(cid, voice, b.peerId(), 8, QList<QByteArray>(8, chunk), false));
    CHECK(until([&] { return videoChunks == 8; }));
    CHECK(video == chunk.repeated(8));
    QObject::disconnect(receive);
}

// Expects `a` sharing to `b` in `voice`, as largeScreenFrameDelivers leaves them.
void shareSoundOvertakesLargeFrames(Node& a, Node& b, const QString& cid, const QString& voice) {
    int videoChunks = 0, chunksBeforeSound = -1;
    const auto receive = QObject::connect(&b, &Node::packetReceived,
                                          [&](const QString&, const QString&, const QString&,
                                              const QString& kind, const QJsonObject&) {
                                              if (kind == "screen")
                                                  ++videoChunks;
                                              if (kind == "screen-audio")
                                                  chunksBeforeSound = videoChunks;
                                          });
    const QByteArray chunk(16 * 1024 - 256, 'v');
    CHECK(a.sendScreenFrame(cid, voice, b.peerId(), 9, QList<QByteArray>(32, chunk), true));
    CHECK(a.sendPacket(cid, voice, "screen-audio",
                       {{"target", b.peerId()}, {"bytes", QString::fromLatin1("c291bmQ=")}}) == 1);
    CHECK(until([&] { return videoChunks == 32 && chunksBeforeSound >= 0; }));
    // Video is fed to the socket a chunk at a time, so the sound goes out
    // next to the start of the frame rather than after all of it.
    CHECK(chunksBeforeSound <= 2);
    QObject::disconnect(receive);
}

void shareSoundReachesOnlyListeningWatchers(Node& a, Node& b, const QString& cid,
                                            const QString& voice) {
    QList<QByteArray> heard;
    const auto receive = QObject::connect(&b, &Node::packetReceived,
                                          [&](const QString&, const QString&, const QString&,
                                              const QString& kind, const QJsonObject& data) {
                                              if (kind == "screen-audio")
                                                  heard += QByteArray::fromBase64(
                                                      data.value("bytes").toString().toLatin1());
                                          });
    const auto send = [&](const QByteArray& bytes, bool targeted = true) {
        QJsonObject data{{"bytes", QString::fromLatin1(bytes.toBase64())}};
        if (targeted)
            data["target"] = b.peerId();
        a.sendPacket(cid, voice, "screen-audio", data);
    };
    // Packets on a link arrive in order, so a valid one after the rejected
    // ones shows that they were dropped.
    send({});
    send(QByteArray(1501, 'x'));
    send("untargeted", false);
    send("first");
    CHECK(until([&] { return !heard.isEmpty(); }));
    CHECK(heard == QList<QByteArray>{"first"});

    b.setVoice(cid, voice, false, true);
    send("while deafened");
    a.setVoice(cid, voice);
    // The end of the share arrives after the packet sent while deafened.
    CHECK(until([&] { return !b.presence(cid, a.peerId()).sharing; }));
    b.setVoice(cid, voice);
    send("after the share");
    a.setVoice(cid, voice, false, false, true);
    CHECK(until([&] { return b.presence(cid, a.peerId()).sharing; }));
    send("last");
    CHECK(until([&] { return heard.size() == 2; }));
    CHECK(heard == (QList<QByteArray>{"first", "last"}));
    QObject::disconnect(receive);
}

void spritesUseAuthenticatedCommunityLinks(Node& a, Node& b, const QString& cid) {
    QList<QJsonObject> received;
    const auto connection = QObject::connect(&b, &Node::packetReceived,
        [&](const QString& community, const QString&, const QString& peer,
            const QString& kind, const QJsonObject& data) {
            if (community == cid && kind == "sprite") {
                CHECK(peer == a.peerId());
                received.append(data);
            }
        });
    const QString text; // Sprite updates belong to the community, independently of channels.
    // Malformed, oversized, and incorrectly targeted chunks must never reach the UI.
    a.sendPacket(cid, text, "sprite", {{"count", 23}, {"index", 0}, {"transfer", 1}, {"bytes", "YQ=="}});
    a.sendPacket(cid, text, "sprite", {{"count", 1}, {"index", -1}, {"transfer", 1}, {"bytes", "YQ=="}});
    a.sendPacket(cid, text, "sprite", {{"count", 1}, {"index", 0}, {"transfer", 1},
        {"bytes", QString::fromLatin1(QByteArray(12 * 1024 + 1, 'x').toBase64())}});
    a.sendPacket(cid, text, "sprite", {{"count", 1}, {"index", 0}, {"transfer", -1}, {"bytes", "YQ=="}});
    CHECK(a.sendPacket(cid, text, "sprite", {{"count", 0}, {"target", freshId()}}) == 0);
    const auto bytes = QByteArray(12 * 1024, 'x');
    CHECK(a.sendPacket(cid, text, "sprite", {{"count", 2}, {"index", 0}, {"transfer", 123},
        {"target", b.peerId()}, {"bytes", QString::fromLatin1(bytes.toBase64())}}) >= 1);
    CHECK(a.sendPacket(cid, text, "sprite", {{"count", 2}, {"index", 1}, {"transfer", 123},
        {"target", b.peerId()}, {"bytes", "YQ=="}}) >= 1);
    CHECK(a.sendPacket(cid, text, "sprite", {{"count", 0}, {"target", b.peerId()}}) >= 1);
    CHECK(until([&] { return received.size() >= 3; }));
    CHECK(received.size() == 3);
    CHECK(QByteArray::fromBase64(received[0].value("bytes").toString().toLatin1()) == bytes);
    CHECK(received[1].value("index").toInt() == 1);
    CHECK(received[2].value("count").toInt() == 0);
    QObject::disconnect(connection);
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
    spritesUseAuthenticatedCommunityLinks(a, b, cid);
    CHECK(a.connectionCount() == 1 && b.connectionCount() == 1);
    const auto second = a.createCommunity("two");
    b.joinCommunity(a.invite(second));
    CHECK(until([&] { return b.online(second).size() == 2 && a.online(second).size() == 2; }));
    CHECK(a.connectionCount() == 1 && b.connectionCount() == 1);
    b.setProfile({"Bobby", "Live music & late-night chats.", QByteArray("avatar")});
    CHECK(until([&] {
        const auto first = a.communities()[cid].members[b.peerId()];
        const auto other = a.communities()[second].members[b.peerId()];
        return first.name == "Bobby" && first.bio == b.profile().bio &&
               first.avatar == b.profile().avatar && other.name == first.name &&
               other.bio == first.bio && other.avatar == first.avatar && !a.synchronizing(cid) &&
               !a.synchronizing(second);
    }));
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
    largeScreenFrameDelivers(a, b, cid, voice);
    shareSoundOvertakesLargeFrames(a, b, cid, voice);
    shareSoundReachesOnlyListeningWatchers(a, b, cid, voice);
}

void stalledAuthenticatedMetadataDoesNotBlockAdmissionsForever() {
    QTemporaryDir ad, bd, cd;
    Node a(ad.path()), b(bd.path()), c(cd.path());
    CHECK(a.start("alice", "127.0.0.1") && b.start("bob", "127.0.0.1") &&
          c.start("carol", "127.0.0.1"));
    const auto cid = a.createCommunity("bounded metadata wait");
    const auto invitation = a.invite(cid);
    b.joinCommunity(invitation);
    CHECK(until([&] {
        return a.online(cid).size() == 2 && b.online(cid).size() == 2 &&
               !a.synchronizing(cid) && !b.synchronizing(cid);
    }));
    const auto voice = channel(a.communities()[cid], "voice");
    a.setVoice(cid, voice);
    b.setVoice(cid, voice);
    CHECK(until([&] { return a.presence(cid, b.peerId()).voiceChannel == voice; }));
    for (auto* socket : b.findChildren<QSslSocket*>())
        socket->blockSignals(true);
    a.edit(cid, "rename", {{"name", "unacknowledged update"}});
    CHECK(a.synchronizing(cid));
    c.joinCommunity(invitation);
    CHECK(until([&] { return a.communities()[cid].isMember(c.peerId()); }, 24000));
    // The stalled transfer no longer blocks admission, but it must not take
    // the peer link or its voice presence down with it.
    CHECK(a.online(cid).contains(b.peerId()));
    CHECK(a.presence(cid, b.peerId()).voiceChannel == voice);
}

void stalledUnauthenticatedMemberDoesNotBlockAdmissions() {
    QTemporaryDir ad, bd, cd;
    Node a(ad.path()), b(bd.path()), c(cd.path());
    CHECK(a.start("alice", "127.0.0.1") && b.start("bob", "127.0.0.1") &&
          c.start("carol", "127.0.0.1"));
    const auto cid = a.createCommunity("unreachable member");
    const auto invitation = a.invite(cid);
    b.joinCommunity(invitation);
    CHECK(until([&] {
        return a.online(cid).size() == 2 && b.online(cid).size() == 2 &&
               !a.synchronizing(cid) && !b.synchronizing(cid);
    }));

    const auto oldPort = b.port();
    b.stop();
    QTcpServer stalled;
    CHECK(stalled.listen(QHostAddress::LocalHost, oldPort));
    CHECK(until([&] { return stalled.hasPendingConnections(); }));
    auto* socket = stalled.nextPendingConnection();
    CHECK(socket != nullptr);
    // This endpoint accepts TCP but never authenticates as Bob. It must not
    // suspend all admissions while Alice repeatedly retries the stale route.
    c.joinCommunity(invitation);
    CHECK(until([&] { return c.online(cid).size() == 2 && a.online(cid).size() == 2; }, 4000));
    CHECK(a.communities()[cid].isMember(c.peerId()));
}

void localAuthorityDoesNotDependOnPeerSynchronization() {
    QTemporaryDir ad, bd;
    Node a(ad.path()), b(bd.path());
    CHECK(a.start("alice", "127.0.0.1") && b.start("bob", "127.0.0.1"));
    const auto cid = a.createCommunity("local authority");
    b.joinCommunity(a.invite(cid));
    CHECK(until([&] {
        return a.online(cid).size() == 2 && b.online(cid).size() == 2 &&
               !a.synchronizing(cid) && !b.synchronizing(cid);
    }));

    // Bob's live authenticated connection stops consuming metadata. Alice
    // retains authority while her first signed update awaits acknowledgement.
    const auto bobSockets = b.findChildren<QSslSocket*>();
    CHECK(!bobSockets.isEmpty());
    for (auto* socket : bobSockets)
        socket->blockSignals(true);
    a.edit(cid, "rename", {{"name", "first local edit"}});
    CHECK(a.synchronizing(cid));
    CHECK(a.communities()[cid].canAdmin(a.peerId()));
    a.edit(cid, "rename", {{"name", "owner remains in control"}});
    CHECK(Invite::fromLink(a.invite(cid)).community == cid);
    CHECK(a.communities()[cid].name == "owner remains in control");

    // A member also retains the right to leave when a different peer is
    // unresponsive, including while their own profile update is unacknowledged.
    for (auto* socket : a.findChildren<QSslSocket*>())
        socket->blockSignals(true);
    b.setProfile({"bob", "unsynchronized profile", {}});
    CHECK(b.synchronizing(cid));
    CHECK(!b.communities()[cid].canAdmin(b.peerId()));
    b.edit(cid, "leave", {});
    CHECK(!b.communities()[cid].isMember(b.peerId()));

    // Restore both links and verify signed concurrent updates still converge.
    for (auto* socket : a.findChildren<QSslSocket*>()) {
        socket->blockSignals(false);
        QMetaObject::invokeMethod(socket, "readyRead", Qt::QueuedConnection);
    }
    for (auto* socket : bobSockets) {
        socket->blockSignals(false);
        QMetaObject::invokeMethod(socket, "readyRead", Qt::QueuedConnection);
    }
    CHECK(until([&] { return !a.communities()[cid].isMember(b.peerId()); }));
    CHECK(a.communities()[cid].name == "owner remains in control");
}

void metadataUpdatesKeepMembersOnlineAndVoiceConnected() {
    QTemporaryDir ad, bd;
    Node a(ad.path()), b(bd.path());
    diagnostics(a);
    diagnostics(b);
    CHECK(a.start("alice", "127.0.0.1") && b.start("bob", "127.0.0.1"));
    const auto cid = a.createCommunity("continuous presence");
    b.joinCommunity(a.invite(cid));
    CHECK(until([&] {
        return a.online(cid).size() == 2 && b.online(cid).size() == 2 &&
               !a.synchronizing(cid) && !b.synchronizing(cid);
    }));
    const auto voice = channel(a.communities()[cid], "voice");
    a.setVoice(cid, voice);
    b.setVoice(cid, voice);
    CHECK(until([&] { return a.presence(cid, b.peerId()).voiceChannel == voice; }));
    bool disappeared = false;
    const auto observe = [&] {
        disappeared |= a.online(cid).size() != 2 || b.online(cid).size() != 2;
    };
    QObject::connect(&a, &Node::changed, observe);
    QObject::connect(&b, &Node::changed, observe);
    a.edit(cid, "rename", {{"name", "renamed without interruption"}});
    observe();
    CHECK(until([&] {
        return b.communities()[cid].name == "renamed without interruption" &&
               !a.synchronizing(cid) && !b.synchronizing(cid);
    }));
    CHECK(!disappeared);
    CHECK(a.presence(cid, b.peerId()).voiceChannel == voice);
    CHECK(b.presence(cid, a.peerId()).voiceChannel == voice);
}

void voiceMovesRequireAdminAndPreserveMuteState() {
    QTemporaryDir ad, bd;
    Node a(ad.path()), b(bd.path());
    CHECK(a.start("alice", "127.0.0.1") && b.start("bob", "127.0.0.1"));
    const auto cid = a.createCommunity("voice moves");
    b.joinCommunity(a.invite(cid));
    CHECK(until([&] {
        return a.online(cid).size() == 2 && b.online(cid).size() == 2 &&
               !a.synchronizing(cid) && !b.synchronizing(cid);
    }));
    const auto voice = channel(a.communities()[cid], "voice");
    const auto destination = freshId();
    a.edit(cid, "channel", {{"id", destination}, {"name", "Moved"}, {"type", "voice"}, {"topic", ""}});
    CHECK(until([&] { return b.communities()[cid].channels.contains(destination) && !a.synchronizing(cid); }));
    a.setVoice(cid, voice);
    b.setVoice(cid, voice, true, true, true);
    CHECK(until([&] { return a.presence(cid, b.peerId()).voiceChannel == voice; }));
    bool denied = false;
    try { b.moveVoice(cid, a.peerId(), destination); }
    catch (const std::exception&) { denied = true; }
    CHECK(denied);
    CHECK(a.presence(cid, a.peerId()).voiceChannel == voice);
    int moved = 0;
    QObject::connect(&b, &Node::voiceMoved, [&](const QString& community, const QString& channel) {
        CHECK(community == cid && channel == destination);
        ++moved;
    });
    a.moveVoice(cid, b.peerId(), destination);
    CHECK(until([&] { return moved == 1 && a.presence(cid, b.peerId()).voiceChannel == destination; }));
    CHECK(b.presence(cid, b.peerId()).muted && b.presence(cid, b.peerId()).deafened);
    CHECK(!b.presence(cid, b.peerId()).sharing);
}

void rejoinToleratesAnInFlightSnapshot() {
    QTemporaryDir ad, bd;
    Node a(ad.path()), b(bd.path());
    CHECK(a.start("alice", "127.0.0.1") && b.start("bob", "127.0.0.1"));
    diagnostics(b);
    const auto cid = a.createCommunity("in-flight update");
    const auto invitation = a.invite(cid);
    b.joinCommunity(invitation);
    CHECK(until([&] {
        return a.online(cid).size() == 2 && b.online(cid).size() == 2 &&
               !a.synchronizing(cid) && !b.synchronizing(cid);
    }));
    QStringList errors;
    int joined = 0;
    QObject::connect(&b, &Node::errorOccurred,
                     [&](const QString& message) { errors.append(message); });
    QObject::connect(&b, &Node::joined, [&](const QString&) { ++joined; });
    const auto sockets = b.findChildren<QSslSocket*>();
    CHECK(sockets.size() == 1);
    auto* socket = sockets.front();
    socket->blockSignals(true);
    a.edit(cid, "rename", {{"name", "in-flight update"}});
    CHECK(until([&] {
        auto buffered = socket->peek(socket->bytesAvailable());
        QJsonObject message;
        while (takeFrame(buffered, message))
            if (message.value("type") == "state-end" && message.value("community") == cid)
                return true;
        return false;
    }));
    // A metadata snapshot from before the leave arrives during the new join.
    b.edit(cid, "leave", {});
    b.joinCommunity(invitation);
    socket->blockSignals(false);
    CHECK(QMetaObject::invokeMethod(socket, "readyRead", Qt::DirectConnection));
    CHECK(errors.isEmpty());
    CHECK(joined == 0 && b.pendingCommunities().contains(cid));
    CHECK(until([&] { return a.online(cid).size() == 2 && b.online(cid).size() == 2; }));
    CHECK(errors.isEmpty() && joined == 1);
    CHECK(a.connectionCount() == 1 && b.connectionCount() == 1);
}

void repeatedRejoinUsesExistingPeerConnection() {
    QTemporaryDir ad, bd;
    Node a(ad.path()), b(bd.path());
    diagnostics(a);
    diagnostics(b);
    CHECK(a.start("alice", "127.0.0.1") && b.start("bob", "127.0.0.1"));
    const auto cid = a.createCommunity("rejoining");
    const auto invitation = a.invite(cid);
    b.joinCommunity(invitation);
    CHECK(until([&] { return a.online(cid).size() == 2 && b.online(cid).size() == 2; }));
    const auto shared = a.createCommunity("keep the connection");
    b.joinCommunity(a.invite(shared));
    CHECK(until([&] { return a.online(shared).size() == 2 && b.online(shared).size() == 2; }));
    for (int attempt = 0; attempt < 3; ++attempt) {
        b.edit(cid, "leave", {});
        if (attempt == 0)
            CHECK(until([&] { return !a.communities()[cid].isMember(b.peerId()); }));
        // Later attempts deliberately rejoin before the previous leave is acknowledged.
        b.joinCommunity(invitation);
        CHECK(until([&] { return a.online(cid).size() == 2 && b.online(cid).size() == 2; }));
        CHECK(a.online(shared).size() == 2 && b.online(shared).size() == 2);
        CHECK(a.connectionCount() == 1 && b.connectionCount() == 1);
    }
}

void leavingAndDeletingPropagateWithoutInterruptingOtherCommunities() {
    QTemporaryDir ad, bd;
    Node a(ad.path()), b(bd.path());
    CHECK(a.start("alice", "127.0.0.1") && b.start("bob", "127.0.0.1"));
    const auto first = a.createCommunity("departing");
    b.joinCommunity(a.invite(first));
    CHECK(until([&] { return a.online(first).size() == 2 && !a.synchronizing(first); }));
    const auto second = a.createCommunity("remaining");
    b.joinCommunity(a.invite(second));
    CHECK(until([&] {
        return a.online(second).size() == 2 && b.online(second).size() == 2 &&
               !b.synchronizing(first) && !a.synchronizing(second);
    }));
    b.edit(first, "leave", {});
    CHECK(until([&] { return !a.communities()[first].isMember(b.peerId()); }));
    CHECK(a.online(second).size() == 2 && b.online(second).size() == 2);
    a.edit(second, "delete", {});
    CHECK(until([&] { return b.communities()[second].deleted; }));
    CHECK(a.online(second).isEmpty() && b.online(second).isEmpty());
    CHECK(!b.communities()[second].canAdmin(a.peerId()));

    const auto offline = a.createCommunity("deleted while member offline");
    b.joinCommunity(a.invite(offline));
    CHECK(until([&] { return b.online(offline).size() == 2 && !a.synchronizing(offline); }));
    b.stop();
    CHECK(until([&] { return a.online(offline).size() == 1; }));
    a.stop();
    CHECK(!a.synchronizing(offline));
    CHECK(a.communities()[offline].canAdmin(a.peerId()));
    CHECK(Invite::fromLink(a.invite(offline)).community == offline);
    a.edit(offline, "delete", {});
    CHECK(a.start("alice", "127.0.0.1"));
    CHECK(b.start("bob", "127.0.0.1"));
    CHECK(until([&] { return b.communities()[offline].deleted; }));
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
    QTcpSocket oldProtocol;
    oldProtocol.connectToHost("127.0.0.1", relay.port());
    CHECK(oldProtocol.waitForConnected());
    oldProtocol.write(QByteArray::fromHex("000000050100000000"));
    CHECK(until([&] { return oldProtocol.state() == QAbstractSocket::UnconnectedState; }));
    CHECK(oldProtocol.readAll().isEmpty());
    const auto endpoint = "relay://127.0.0.1:" + QString::number(relay.port());
    QTemporaryDir ad, bd, cd;
    Node a(ad.path()), b(bd.path()), c(cd.path());
    diagnostics(a);
    diagnostics(b);
    diagnostics(c);
    CHECK(a.start("alice", {}, 0, endpoint) && b.start("bob", {}, 0, endpoint) &&
          c.start("carol", {}, 0, endpoint));
    CHECK(a.port() == 0 && b.port() == 0); // relay mode never opens a direct listener
    const auto cid = a.createCommunity("relayed");
    const auto link = a.invite(cid);
    b.joinCommunity(link);
    CHECK(until([&] { return a.online(cid).size() == 2 && b.online(cid).size() == 2; }));
    spritesUseAuthenticatedCommunityLinks(a, b, cid);
    bool chatReceived = false;
    QObject::connect(&b, &Node::packetReceived,
                     [&](const QString& community, const QString&, const QString&,
                         const QString& kind, const QJsonObject& packet) {
                         if (community == cid && kind == "chat" &&
                             packet.value("body") == "community through relay")
                             chatReceived = true;
                     });
    CHECK(a.sendPacket(cid, channel(a.communities()[cid], "text"), "chat",
                       {{"body", "community through relay"}}) == 1);
    CHECK(until([&] { return chatReceived; }));
    CHECK(until([&] { return a.relayConnected() && b.relayConnected(); }));
    relay.interrupt();
    CHECK(until(
        [&] { return !a.relayConnected() && !b.relayConnected() && b.online(cid).size() == 1; }));
    relay.resume();
    const auto singleTunnel = [](const Node& node) {
        int active = 0;
        for (auto* socket : node.findChildren<QSslSocket*>())
            if (socket->state() != QAbstractSocket::UnconnectedState)
                ++active;
        return active == 1;
    };
    // Concurrent redials can report both peers online before duplicate TLS
    // tunnels are resolved. Check delivery only after that replacement settles.
    CHECK(until(
        [&] {
            return a.relayConnected() && b.relayConnected() && a.online(cid).size() == 2 &&
                   b.online(cid).size() == 2 && singleTunnel(a) && singleTunnel(b);
        },
        20000));
    chatReceived = false;
    CHECK(a.sendPacket(cid, channel(a.communities()[cid], "text"), "chat",
                       {{"body", "community through relay"}}) == 1);
    CHECK(until([&] { return chatReceived; }));
    largeScreenFrameDelivers(a, b, cid, channel(a.communities()[cid], "voice"));
    shareSoundOvertakesLargeFrames(a, b, cid, channel(a.communities()[cid], "voice"));
    shareSoundReachesOnlyListeningWatchers(a, b, cid, channel(a.communities()[cid], "voice"));
    a.stop();
    CHECK(a.communities()[cid].canAdmin(a.peerId()));
    CHECK(until([&] { return b.online(cid).size() == 1; }));
    CHECK(b.communities()[cid].canAdmin(a.peerId()));
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
        return a.communities()[cid].name == "admin acting alone" && a.online(cid).size() == 3 &&
               !a.synchronizing(cid);
    }));
    CHECK(a.communities()[cid].canAdmin(a.peerId()));
    a.edit(cid, "rename", {{"name", "owner still has authority"}});
    CHECK(until([&] { return b.communities()[cid].name == "owner still has authority"; }));
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
        b.setProfile({"bob", "Edited while disconnected", QByteArray("offline avatar")});
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
        CHECK(until([&] {
            return a.communities()[cid].members[b.peerId()].bio == "Edited while disconnected";
        }));
        CHECK(messages == 0);
        CHECK(a.sendPacket(cid, text, "chat", {{"body", "live after reconnect"}}) == 1);
        CHECK(until([&] { return messages == 1; }));
    }
}
} // namespace
int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    const bool metadataOnly = app.arguments().contains("--metadata-sync");
    if (argc == 2 && !metadataOnly)
        relayExecutable = QString::fromLocal8Bit(argv[1]);
    const auto backend = qEnvironmentVariable("DISQUISITION_TEST_TLS_BACKEND");
    if (!backend.isEmpty() && !QSslSocket::setActiveBackend(backend)) {
        std::fprintf(stderr, "TLS backend unavailable: %s\n", backend.toUtf8().constData());
        return 77;
    }
    if (metadataOnly) {
        metadataValidationYieldsToTheEventLoop();
        std::puts("community_metadata_sync_test: ok");
        return 0;
    }
    rejoinToleratesAnInFlightSnapshot();
    stalledAuthenticatedMetadataDoesNotBlockAdmissionsForever();
    stalledUnauthenticatedMemberDoesNotBlockAdmissions();
    localAuthorityDoesNotDependOnPeerSynchronization();
    relayRejectsInvalidAndRetiredOptions();
    directAdmissionIsolationAndConnectionReuse();
    directListenerReportsRelayAddressMixupsWithoutTlsErrors();
    relayDiagnosticsAreReportedOnceAcrossRetries();
    metadataUpdatesKeepMembersOnlineAndVoiceConnected();
    voiceMovesRequireAdminAndPreserveMuteState();
    repeatedRejoinUsesExistingPeerConnection();
    leavingAndDeletingPropagateWithoutInterruptingOtherCommunities();
    relayHostMigrationAndReconnection();
    returningHostSynchronizesBeforeAcceptingInvites();
    preferredHostCoordinatesAdmissions();
    allOfflineRestoresMetadataAndNeverReplaysMissedChat();
    std::puts("community_network_test: ok");
}
