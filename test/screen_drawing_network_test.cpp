#include <QCoreApplication>
#include <QElapsedTimer>
#include <QProcess>
#include <QRegularExpression>
#include <QThread>
#include <cstdio>
#include <functional>

#include "check.h"
#include "client/connection.h"
#include "desktop/screen_drawing_session.h"

namespace {
QProcess* serverProcess = nullptr;
QProcess* relayProcess = nullptr;
void cleanup() {
    for (auto* process : {relayProcess, serverProcess}) {
        if (process && process->state() != QProcess::NotRunning) {
            process->terminate();
            if (!process->waitForFinished(3000)) {
                process->kill();
                process->waitForFinished(3000);
            }
        }
    }
}

bool waitFor(const std::function<bool()>& condition) {
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < 10000) {
        QCoreApplication::processEvents();
        if (condition())
            return true;
        QThread::msleep(2);
    }
    return condition();
}

quint16 start(QProcess& process, const QString& program, const QStringList& arguments) {
    process.setProcessChannelMode(QProcess::MergedChannels);
    process.start(program, arguments);
    CHECK(process.waitForStarted(3000));
    QByteArray output;
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < 10000) {
        process.waitForReadyRead(100);
        output += process.readAll();
        const auto match =
            QRegularExpression("listening on port (\\d+)").match(QString::fromUtf8(output));
        if (match.hasMatch())
            return match.captured(1).toUShort();
        if (process.state() == QProcess::NotRunning)
            break;
    }
    std::fprintf(stderr, "%s\n", output.constData());
    CHECK(false);
    return 0;
}

struct Participant {
    chat::Connection connection;
    ScreenDrawing drawing;
    ScreenDrawingSession session;
    std::string name;
    Participant(quint16 port, const QString& localName, bool owner) :
        session(localName, "presenter", &drawing, owner), name(localName.toStdString()) {
        std::string error;
        CHECK(connection.connectTo("127.0.0.1", port, 3000, error));
        connection.startReader();
        CHECK(connection.send({chat::MsgType::Login, {name, "0", ""}}));
        CHECK(waitFor([&] {
            chat::Message message;
            while (connection.poll(message)) {
                if (message.type == chat::MsgType::LoginOk)
                    return message.fields[0] == name;
            }
            return false;
        }));
        QObject::connect(&session, &ScreenDrawingSession::sendToPeer, &session,
                         [this](const QString& target, const chat::Message& message) {
                             CHECK(connection.send(
                                 chat::peerDataMessage({name, target.toStdString(),
                                                        "screen-drawing", chat::encode(message)})));
                         });
    }
    void poll() {
        chat::Message message;
        while (connection.poll(message)) {
            CHECK(message.type != chat::MsgType::Error);
            if (message.type == chat::MsgType::PeerData) {
                chat::PeerData data;
                CHECK(chat::parsePeerData(message, data));
                CHECK(data.target == name && data.channel == "screen-drawing");
                chat::Message payload;
                CHECK(chat::decode(data.data, payload) == chat::DecodeStatus::Ok &&
                      data.data.empty());
                session.receive(QString::fromStdString(data.sender), payload);
            }
        }
    }
};
} // namespace

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    CHECK(argc == 3);
    QProcess server, relay;
    serverProcess = &server;
    relayProcess = &relay;
    gCheckCleanup = cleanup;
    const auto serverPort = start(server, QString::fromLocal8Bit(argv[1]),
                                  {"--port", "0", "--db", ":memory:"});
    const auto relayPort = start(relay, QString::fromLocal8Bit(argv[2]),
                                 {"--host", "127.0.0.1", "--port", QString::number(serverPort),
                                  "--listen", "0"});
    Participant owner(relayPort, "presenter", true), viewer(relayPort, "viewer", false);
    const auto pump = [&] {
        owner.poll();
        viewer.poll();
    };
    owner.drawing.permissionRequested(true);
    CHECK(owner.drawing.enabled()); // enabling never waits for the network
    viewer.session.start();
    CHECK(waitFor([&] {
        pump();
        return viewer.drawing.enabled();
    }));
    chat::ScreenDraw segment;
    segment.id = 1;
    segment.x1 = 100;
    segment.x2 = 9000;
    segment.y1 = 500;
    segment.y2 = 8500;
    viewer.drawing.segmentDrawn(segment);
    CHECK(waitFor([&] {
        pump();
        return owner.drawing.segments().size() == 1;
    }));
    CHECK(owner.drawing.segments()[0].author == "viewer");
    CHECK(viewer.drawing.segments().size() == 1); // canonical echo deduplicates the preview
    chat::ScreenLaser laser{"", "", 0, 10, 7500, 2500, 0xff5470, true};
    viewer.drawing.laserPointed(laser);
    CHECK(waitFor([&] { pump(); return owner.drawing.lasers().contains("viewer"); }));
    CHECK(owner.drawing.lasers()["viewer"].point.x == 7500);
    CHECK(owner.drawing.segments().size() == 1); // no persistent ink from the laser
    laser.id = 11; laser.visible = false;
    viewer.drawing.laserPointed(laser);
    CHECK(waitFor([&] { pump(); return !owner.drawing.lasers()["viewer"].point.visible; }));
    // More history than one relay event-queue batch must survive a late subscription.
    for (int i = 0; i < 600; ++i) {
        segment.id = 100 + i;
        owner.drawing.segmentDrawn(segment);
    }
    CHECK(waitFor([&] {
        pump();
        return viewer.drawing.segments().size() == 601;
    }));
    Participant late(relayPort, "late_viewer", false);
    late.session.start();
    CHECK(waitFor([&] {
        pump();
        late.poll();
        return late.drawing.segments().size() == 601;
    }));
    owner.session.setBanned("viewer", true);
    CHECK(waitFor([&] { pump(); late.poll(); return viewer.drawing.banned(); }));
    CHECK(!late.drawing.banned() && late.drawing.drawingAllowed());
    CHECK(viewer.drawing.enabled()); // the ban does not stop watching
    laser.id = 100; laser.visible = true; laser.sharer = "presenter";
    laser.epoch = owner.drawing.epoch();
    viewer.session.sendToPeer("presenter", chat::screenLaserMessage(laser, false));
    segment.id = 55; segment.stroke = 55; segment.sharer = "presenter";
    segment.epoch = owner.drawing.epoch();
    // Send through the actual relay while bypassing the local drawing controls.
    viewer.session.sendToPeer("presenter", chat::screenDrawMessage(segment, false));
    viewer.session.start(); // ordered after the proposal, confirms canonical history
    CHECK(waitFor([&] {
        pump(); late.poll();
        return viewer.drawing.banned() && viewer.drawing.segments().size() == 601;
    }));
    CHECK(owner.drawing.segments().size() == 601 && late.drawing.segments().size() == 601);
    CHECK(!owner.drawing.lasers().contains("viewer") || owner.drawing.lasers()["viewer"].point.id < 100);
    CHECK(!late.drawing.lasers().contains("viewer"));
    owner.session.setBanned("viewer", false);
    CHECK(waitFor([&] { pump(); late.poll(); return !viewer.drawing.banned(); }));
    viewer.drawing.segmentDrawn(segment);
    CHECK(waitFor([&] {
        pump(); late.poll();
        return owner.drawing.segments().size() == 602 && late.drawing.segments().size() == 602;
    }));
    // Per-author clearing and batched stroke deletion use the same generic
    // peer transport through an unchanged discovery server and relay.
    viewer.drawing.clearMineRequested();
    CHECK(waitFor([&] {
        pump(); late.poll();
        return owner.drawing.segments().size() == 600 && viewer.drawing.segments().size() == 600 &&
               late.drawing.segments().size() == 600;
    }));
    owner.drawing.clearMineRequested(); // more than one erase-message batch
    CHECK(waitFor([&] {
        pump(); late.poll();
        return owner.drawing.segments().isEmpty() && viewer.drawing.segments().isEmpty() &&
               late.drawing.segments().isEmpty();
    }));
    const auto previousEpoch = owner.drawing.epoch();
    owner.drawing.clearRequested();
    CHECK(owner.drawing.segments().isEmpty() && owner.drawing.epoch() != previousEpoch);
    CHECK(waitFor([&] {
        pump();
        late.poll();
        return viewer.drawing.segments().isEmpty() && late.drawing.segments().isEmpty();
    }));
    owner.drawing.permissionRequested(false);
    CHECK(waitFor([&] {
        pump();
        late.poll();
        return !viewer.drawing.enabled() && !late.drawing.enabled();
    }));
    cleanup();
    std::puts("screen_drawing_network_test: ok");
}
