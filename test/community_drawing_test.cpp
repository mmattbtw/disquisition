#include <QApplication>
#include <QDeadlineTimer>
#include <QTcpServer>
#include <QTemporaryDir>
#include <QThread>
#include <functional>

#include "check.h"
#include "community/node.h"
#include "desktop/screen_drawing_session.h"
#include "relay/community_router.h"

static bool until(const std::function<bool()>& condition) {
    QDeadlineTimer deadline(10000);
    while (!deadline.hasExpired()) {
        QApplication::processEvents(QEventLoop::AllEvents, 10);
        if (condition()) return true;
        QThread::msleep(1);
    }
    return condition();
}

class Relay : public QTcpServer {
public:
    Relay() { router.start(); CHECK(listen(QHostAddress::LocalHost, 0)); }
protected:
    void incomingConnection(qintptr socket) override { router.adopt(socket); }
private:
    chat::CommunityRouter router;
};

static void exercise(bool relayed) {
    Relay relay;
    QTemporaryDir ad, bd, cd;
    community::Node a(ad.path()), b(bd.path()), c(cd.path());
    const auto endpoint = relayed ? "127.0.0.1:" + QString::number(relay.serverPort()) : QString();
    for (auto* node : {&a, &b, &c})
        CHECK(node->start("Same name", "127.0.0.1", 0, endpoint));
    const auto cid = a.createCommunity("Drawing");
    b.joinCommunity(a.invite(cid));
    CHECK(until([&] { return a.online(cid).size() == 2 && b.online(cid).size() == 2; }));
    c.joinCommunity(a.invite(cid));
    CHECK(until([&] { return a.online(cid).size() == 3 && b.online(cid).size() == 3 && c.online(cid).size() == 3; }));
    QString voice;
    for (const auto& channel : a.communities()[cid].channels)
        if (channel.type == "voice") voice = channel.id;
    const auto other = community::freshId();
    a.edit(cid, "channel", {{"id", other}, {"name", "Other"}, {"type", "voice"}, {"topic", ""}});
    CHECK(until([&] { return b.communities()[cid].channels.contains(other) && c.communities()[cid].channels.contains(other); }));
    a.setVoice(cid, voice, false, false, true);
    b.setVoice(cid, voice);
    c.setVoice(cid, other);
    CHECK(until([&] { return a.presence(cid, b.peerId()).voiceChannel == voice &&
                            b.presence(cid, a.peerId()).sharing &&
                            a.presence(cid, c.peerId()).voiceChannel == other; }));

    ScreenDrawing ownerInk, viewerInk;
    ScreenDrawingSession owner(a.peerId(), a.peerId(), &ownerInk, true);
    ScreenDrawingSession viewer(b.peerId(), a.peerId(), &viewerInk, false);
    auto wire = [&](community::Node& node, ScreenDrawingSession& session) {
        QObject::connect(&session, &ScreenDrawingSession::sendToPeer, &node,
            [&, source = &node](const QString& target, const chat::Message& message) {
                source->sendPacket(cid, voice, "drawing", {{"target", target},
                    {"bytes", QString::fromLatin1(QByteArray::fromStdString(chat::encode(message)).toBase64())}});
            });
        QObject::connect(&node, &community::Node::packetReceived, &session,
            [&, recipient = &session](const QString& community, const QString& channel,
                                     const QString& sender, const QString& type, const QJsonObject& data) {
                if (community != cid || channel != voice || type != "drawing") return;
                auto bytes = QByteArray::fromBase64(data["bytes"].toString().toLatin1()).toStdString();
                chat::Message message;
                CHECK(chat::decode(bytes, message) == chat::DecodeStatus::Ok && bytes.empty());
                recipient->receive(sender, message);
            });
    };
    wire(a, owner);
    wire(b, viewer);
    ownerInk.permissionRequested(true);
    viewer.start();
    CHECK(until([&] { return viewerInk.drawingAllowed(); }));
    chat::ScreenDraw stroke;
    stroke.id = stroke.stroke = 1;
    stroke.x2 = stroke.y2 = 100;
    viewerInk.segmentDrawn(stroke);
    CHECK(until([&] { return ownerInk.segments().size() == 1 && viewerInk.segments().size() == 1; }));
    CHECK(ownerInk.segments().first().author == b.peerId().toStdString());
    CHECK(a.peerId() != b.peerId()); // Identical names never share an author or ban.
    owner.setBanned(b.peerId(), true);
    CHECK(until([&] { return viewerInk.banned(); }));
    owner.setBanned(b.peerId(), false);
    CHECK(until([&] { return !viewerInk.banned(); }));
    viewerInk.clearMineRequested();
    CHECK(until([&] { return ownerInk.segments().isEmpty() && viewerInk.segments().isEmpty(); }));
    CHECK(a.sendPacket(cid, voice, "drawing", {{"target", c.peerId()}, {"bytes", "ignored"}}) == 0);
    b.setVoice(cid, other);
    CHECK(until([&] { return a.presence(cid, b.peerId()).voiceChannel == other; }));
    CHECK(a.sendPacket(cid, voice, "drawing", {{"target", b.peerId()}, {"bytes", "ignored"}}) == 0);
}

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    exercise(false);
    exercise(true);
    std::puts("community_drawing_test: ok");
}
