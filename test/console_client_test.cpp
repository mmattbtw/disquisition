// Interoperability of the Qt-free console community client with the desktop
// community::Node and relay. Both run on this thread: the console client is
// polled from the same loop that processes Qt events.
#include <QCoreApplication>
#include <QDeadlineTimer>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSslSocket>
#include <QTcpServer>
#include <QTemporaryDir>
#include <QThread>
#include <chrono>
#include <cstdlib>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <sstream>

#include "check.h"
#include "community/identity.h"
#include "community/node.h"
#include "console/client.h"
#include "console/session.h"
#include "relay/community_router.h"

namespace {
namespace fs = std::filesystem;

class DirectoryStorage final : public console::Storage {
public:
    explicit DirectoryStorage(fs::path root) : root_(std::move(root)) { fs::create_directories(root_); }
    bool read(const std::string& name, std::string& bytes, std::size_t limit) override {
        const auto path = root_ / name;
        if (!fs::exists(path))
            return false;
        CHECK(fs::file_size(path) <= limit);
        std::ifstream file(path, std::ios::binary);
        std::ostringstream contents;
        contents << file.rdbuf();
        bytes = contents.str();
        return true;
    }
    void write(const std::string& name, const std::string& bytes) override {
        const auto temporary = root_ / (name + ".tmp");
        {
            std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
            file << bytes;
            CHECK(file.good());
        }
        fs::rename(temporary, root_ / name);
    }
    void remove(const std::string& name) override { fs::remove(root_ / name); }
    std::vector<std::string> list(const std::string& suffix) override {
        std::vector<std::string> result;
        for (const auto& entry : fs::directory_iterator(root_)) {
            const auto name = entry.path().filename().string();
            if (name.size() >= suffix.size() &&
                name.compare(name.size() - suffix.size(), suffix.size(), suffix) == 0)
                result.push_back(name);
        }
        return result;
    }

private:
    fs::path root_;
};

class SystemClock final : public console::Clock {
public:
    std::uint64_t monotonicMs() override {
        return std::uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(
                                 std::chrono::steady_clock::now().time_since_epoch())
                                 .count());
    }
    std::int64_t wallMs() override {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::system_clock::now().time_since_epoch())
            .count();
    }
};

std::vector<console::Client*> gConsoles;
std::vector<console::Event> gEvents;

bool until(const std::function<bool()>& predicate, int timeout = 15000) {
    QDeadlineTimer deadline(timeout);
    while (!deadline.hasExpired()) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
        for (auto* console : gConsoles) {
            console->poll();
            for (auto& event : console->takeEvents()) {
                if (event.kind == console::Event::Kind::Error)
                    std::fprintf(stderr, "console: %s\n", event.message.c_str());
                gEvents.push_back(std::move(event));
            }
        }
        if (predicate())
            return true;
        QThread::msleep(1);
    }
    return predicate();
}

class Relay final : public QTcpServer {
public:
    Relay() {
        router.start();
        CHECK(listen(QHostAddress::LocalHost, 0));
    }
    QString address() const { return "127.0.0.1:" + QString::number(serverPort()); }

protected:
    void incomingConnection(qintptr descriptor) override { router.adopt(descriptor); }

private:
    chat::CommunityRouter router;
};

QString channelOf(const community::Model& model, const QString& type) {
    for (const auto& channel : model.channels)
        if (channel.type == type)
            return channel.id;
    return {};
}

std::string text(const QString& value) {
    return value.toStdString();
}

// Signed metadata is verified over Qt's serialization, so both sides must
// produce identical canonical bytes, signatures and invites.
void canonicalFormsMatchTheDesktop() {
    QString lone;
    lone.append(QChar(0xd800));
    const QJsonObject tricky{
        {"quote", "a \"b\" \\c\\ /d"},
        {"controls", QString("\b\f\n\r\t") + QChar(0x01) + QChar(0x1f) + QChar(0x7f)},
        {"unicode", QString::fromUtf8("caf\xc3\xa9 \xe2\x9c\x93 \xf0\x9f\x90\xa7 \xe2\x80\xa8")},
        {"lone", lone},
        {"numbers", QJsonArray{0, -7, 8192, 1763700000000.0, 2147483648.0}},
        {"nested", QJsonObject{{"z", true}, {"a", QJsonValue::Null}, {"m", QJsonArray{}}}},
        {"empty", QJsonObject{}}};
    const auto desktop = community::canonical(tricky);
    const auto parsed = console::json::parse(desktop.toStdString());
    CHECK(console::json::canonical(parsed) == desktop.toStdString());

    const auto identity = community::Identity::generate();
    const auto mine = console::Identity::fromSecret(identity.secret().toStdString());
    CHECK(mine.id() == text(identity.id()));
    const QByteArray message = "disquisition interop";
    CHECK(mine.sign(message.toStdString()) == text(identity.sign(message)));
    CHECK(console::Identity::verify(mine.id(), message.toStdString(), text(identity.sign(message))));
    CHECK(!console::Identity::verify(mine.id(), "tampered", text(identity.sign(message))));

    const auto signedByDesktop = community::signedObject(tricky, identity, "domain\n");
    const auto object = console::json::parse(
        QJsonDocument(signedByDesktop).toJson(QJsonDocument::Compact).toStdString());
    CHECK(console::verifyObject(object.toObject(), mine.id(), "domain\n"));
    const auto signedByConsole = console::signedObject(parsed.toObject(), mine, "domain\n");
    const auto back = QJsonDocument::fromJson(
                          QByteArray::fromStdString(console::json::write(signedByConsole)))
                          .object();
    CHECK(community::verifyObject(back, identity.id(), "domain\n"));
}

void relayJoinChatVoiceAndHosting() {
    Relay relay;
    QTemporaryDir ad, bd, cd;
    community::Node alice(ad.path());
    CHECK(alice.start("alice", {}, 0, relay.address()));
    CHECK(until([&] { return alice.relayConnected(); }));
    const auto cid = alice.createCommunity("Handhelds");
    const auto general = channelOf(alice.communities()[cid], "text");
    const auto lounge = channelOf(alice.communities()[cid], "voice");

    // The invite parses identically on both sides.
    const auto link = alice.invite(cid);
    const auto desktopInvite = community::Invite::fromLink(link);
    const auto consoleInvite = console::Invite::fromLink(text(link));
    CHECK(consoleInvite.community == text(desktopInvite.community) &&
          consoleInvite.owner == text(desktopInvite.owner) &&
          consoleInvite.code == text(desktopInvite.code) &&
          consoleInvite.seeds.size() == std::size_t(desktopInvite.seeds.size()) &&
          consoleInvite.seeds[0]["endpoint"].toString() ==
              text(desktopInvite.seeds[0].toObject().value("endpoint").toString()));

    DirectoryStorage storage(fs::path(text(cd.path())) / "console");
    SystemClock clock;
    console::Client handheld(storage, clock);
    gConsoles = {&handheld};
    handheld.load();
    const std::string name = "3DS \xe2\x9c\x93";
    CHECK(handheld.start(name, text(relay.address())));
    CHECK(until([&] { return handheld.relayConnected(); }));
    const auto me = QString::fromStdString(handheld.peerId());
    handheld.joinCommunity(text(link));
    CHECK(until([&] {
        return handheld.communities().count(text(cid)) &&
               handheld.communities().at(text(cid)).isMember(handheld.peerId()) &&
               alice.communities()[cid].isMember(me) && alice.online(cid).contains(me) &&
               handheld.online(text(cid)).count(text(alice.peerId()));
    }));
    CHECK(alice.communities()[cid].members[me].name == QString::fromStdString(name));
    CHECK(alice.communities()[cid].members[me].endpoint ==
          QString::fromStdString(handheld.endpoint()));
    CHECK(handheld.communities().at(text(cid)).channels.size() == 2);
    CHECK(handheld.communities().at(text(cid)).revision() == text(alice.communities()[cid].revision()));
    CHECK(storage.list(".community.json").size() == 1);

    // Live chat in both directions.
    QJsonObject received;
    QObject::connect(&alice, &community::Node::packetReceived,
                     [&](const QString&, const QString&, const QString& peer, const QString& type,
                         const QJsonObject& data) {
                         if (peer == me)
                             received[type] = data;
                     });
    CHECK(handheld.sendPacket(text(cid), text(general), "chat",
                              {{"body", "hello from a handheld \xf0\x9f\x8e\xae"},
                               {"color", "mint"}}) == 1);
    CHECK(until([&] { return received.contains("chat"); }));
    CHECK(received["chat"].toObject().value("body").toString() ==
          QString::fromUtf8("hello from a handheld \xf0\x9f\x8e\xae"));
    gEvents.clear();
    alice.sendPacket(cid, general, "chat", {{"body", "welcome"}, {"color", "pink"}});
    CHECK(until([&] {
        for (const auto& event : gEvents)
            if (event.kind == console::Event::Kind::Packet && event.type == "chat" &&
                event.data["body"].toString() == "welcome" && event.peer == text(alice.peerId()))
                return true;
        return false;
    }));

    // Voice presence and 20 ms PCM frames in both directions.
    alice.setVoice(cid, lounge);
    handheld.setVoice(text(cid), text(lounge));
    CHECK(until([&] {
        return alice.presence(cid, me).voiceChannel == lounge &&
               handheld.presence(text(cid), text(alice.peerId())).voiceChannel == text(lounge);
    }));
    QByteArray pcm(640, '\x11');
    gEvents.clear();
    CHECK(alice.sendPacket(cid, lounge, "audio", {{"bytes", QString::fromLatin1(pcm.toBase64())}}) ==
          1);
    CHECK(until([&] {
        for (const auto& event : gEvents)
            if (event.kind == console::Event::Kind::Packet && event.type == "audio" &&
                console::fromBase64(event.data["bytes"].toString()) == pcm.toStdString())
                return true;
        return false;
    }));
    CHECK(handheld.sendPacket(text(cid), text(lounge), "audio",
                              {{"bytes", console::base64(std::string(640, '\x22'))}}) == 1);
    CHECK(until([&] { return received.contains("audio"); }));
    CHECK(QByteArray::fromBase64(received["audio"].toObject().value("bytes").toString().toLatin1()) ==
          QByteArray(640, '\x22'));
    handheld.setVoice(text(cid), {});
    CHECK(until([&] { return alice.presence(cid, me).voiceChannel.isEmpty(); }));

    // A console that is not the preferred host validates the invite, then
    // redirects the joiner to the owner before closing.
    {
        auto bootstrap = community::Invite::fromLink(alice.invite(cid));
        bootstrap.seeds.prepend(QJsonObject{{"peer", me},
                                            {"endpoint", QString::fromStdString(handheld.endpoint())}});
        CHECK(until([&] {
            return handheld.communities().at(text(cid)).revision() ==
                   text(alice.communities()[cid].revision());
        }));
        QTemporaryDir ed;
        community::Node erin(ed.path());
        CHECK(erin.start("erin", {}, 0, relay.address()));
        CHECK(until([&] { return erin.relayConnected(); }));
        erin.joinCommunity(bootstrap.toLink());
        CHECK(until([&] {
            return erin.communities().contains(cid) && erin.communities()[cid].isMember(erin.peerId()) &&
                   handheld.communities().at(text(cid)).isMember(text(erin.peerId()));
        }, 10000));
        erin.stop();
    }

    // With the owner offline, the console is the only online member. The relay
    // routes a new member's join to it, and it admits them from the invite.
    const auto second = alice.invite(cid);
    CHECK(until([&] {
        return handheld.communities().at(text(cid)).revision() ==
               text(alice.communities()[cid].revision());
    }));
    alice.stop();
    CHECK(until([&] { return handheld.connectionCount() == 0; }));
    community::Node bob(bd.path());
    CHECK(bob.start("bob", {}, 0, relay.address()));
    CHECK(until([&] { return bob.relayConnected(); }));
    bob.joinCommunity(second);
    CHECK(until([&] {
        return bob.communities().contains(cid) && bob.communities()[cid].isMember(bob.peerId()) &&
               handheld.communities().at(text(cid)).isMember(text(bob.peerId())) &&
               bob.online(cid).contains(me);
    }, 30000));
    CHECK(handheld.communities().at(text(cid)).revision() == text(bob.communities()[cid].revision()));

    // The saved identity and metadata survive a restart.
    const auto peer = handheld.peerId();
    handheld.stop();
    console::Client restarted(storage, clock);
    gConsoles = {&restarted};
    restarted.load();
    CHECK(restarted.peerId() == peer);
    CHECK(restarted.communities().at(text(cid)).isMember(text(bob.peerId())));
    CHECK(restarted.start(name, text(relay.address())));
    CHECK(until([&] { return restarted.online(text(cid)).count(text(bob.peerId())); }, 30000));
    gConsoles.clear();
}

void directMemberAndSession() {
    Relay relay;
    QTemporaryDir ad, cd;
    community::Node carol(ad.path());
    CHECK(carol.start("carol", "127.0.0.1"));
    const auto cid = carol.createCommunity("Direct");
    const auto general = channelOf(carol.communities()[cid], "text");
    const auto lounge = channelOf(carol.communities()[cid], "voice");
    const auto link = carol.invite(cid);
    CHECK(link.contains("join/"));

    // The console reaches a direct-mode desktop at its tcp:// endpoint; the
    // desktop reaches the console through the console's relay.
    DirectoryStorage storage(fs::path(text(cd.path())) / "session");
    SystemClock clock;
    console::Session session(storage, clock);
    gConsoles.clear();
    std::vector<std::string> heard;
    session.audioReceived = [&](const std::string& peer, const std::string&) {
        heard.push_back(peer);
    };
    const auto pump = [&](const std::function<bool()>& predicate, int timeout = 15000) {
        return until([&] {
            session.poll();
            return predicate();
        }, timeout);
    };
    CHECK(session.start("Wii U", text(relay.address()), "butter"));
    CHECK(pump([&] { return session.view.relayConnected; }));
    session.join(text(link));
    CHECK(pump([&] {
        return session.view.communities.size() == 1 && !session.view.communities[0].pending &&
               session.view.community == text(cid) && session.view.channel == text(general);
    }));
    CHECK(pump([&] {
        const auto& members = session.view.communities[0].members;
        return members.size() == 2 && members[0].online && members[1].online;
    }));
    CHECK(session.view.communities[0].channels.size() == 2 &&
          session.view.communities[0].channels[0].type == "text" &&
          session.view.communities[0].channels[1].type == "voice");
    QString body;
    QObject::connect(&carol, &community::Node::packetReceived,
                     [&](const QString&, const QString&, const QString&, const QString& type,
                         const QJsonObject& data) {
                         if (type == "chat")
                             body = data.value("body").toString();
                     });
    CHECK(session.sendChat("  from the GamePad  "));
    CHECK(pump([&] { return body == "from the GamePad"; }));
    CHECK(session.view.messages.size() == 1 && session.view.messages[0].local &&
          !session.view.messages[0].undelivered);
    carol.sendPacket(cid, general, "chat", {{"body", "hi wii u"}, {"color", "pink"}});
    CHECK(pump([&] {
        return session.view.messages.size() == 2 && session.view.messages[1].body == "hi wii u" &&
               session.view.messages[1].sender == "carol";
    }));

    carol.setVoice(cid, lounge);
    CHECK(session.joinVoice(text(cid), text(lounge)));
    session.controls(false, false, false, false);
    CHECK(session.view.transmitting);
    CHECK(pump([&] {
        return carol.presence(cid, QString::fromStdString(session.view.peerId)).voiceChannel ==
               lounge;
    }));
    QByteArray loud;
    for (int index = 0; index < 320; ++index) {
        const qint16 sample = index % 2 ? 8000 : -8000;
        loud.append(char(sample & 0xff));
        loud.append(char((sample >> 8) & 0xff));
    }
    carol.sendPacket(cid, lounge, "audio", {{"bytes", QString::fromLatin1(loud.toBase64())}});
    CHECK(pump([&] {
        if (heard.empty())
            return false;
        for (const auto& member : session.view.communities[0].members)
            if (member.id == text(carol.peerId()))
                return member.speaking && member.voiceChannel == text(lounge);
        return false;
    }));
    // Push-to-talk gates frames locally.
    session.controls(false, false, true, false);
    CHECK(!session.view.transmitting);
    session.leaveVoice();
    CHECK(pump([&] {
        return carol.presence(cid, QString::fromStdString(session.view.peerId)).voiceChannel.isEmpty();
    }));
    session.leave(text(cid));
    CHECK(pump([&] {
        return session.view.communities.empty() &&
               !carol.communities()[cid].isMember(QString::fromStdString(session.view.peerId));
    }));
    session.stop("Stopped");
}

// Metadata histories may approach the desktop's 8 MiB limit. A console host
// paces a snapshot larger than its send buffer instead of dropping the link.
void consoleHostsALargeHistory() {
    Relay relay;
    QTemporaryDir ad, bd, cd;
    community::Node alice(ad.path());
    CHECK(alice.start("alice", {}, 0, relay.address()));
    const auto cid = alice.createCommunity("Icons");
    QByteArray icon(128 * 1024, Qt::Uninitialized);
    for (int edit = 0; edit < 28; ++edit) {
        for (auto& byte : icon)
            byte = char(std::rand());
        alice.edit(cid, "icon", {{"bytes", QString::fromLatin1(icon.toBase64())}});
    }
    CHECK(community::canonical(alice.communities()[cid].toJson()).size() > 4 * 1024 * 1024);
    CHECK(until([&] { return alice.relayConnected(); }));
    DirectoryStorage storage(fs::path(text(cd.path())) / "console");
    SystemClock clock;
    console::Client handheld(storage, clock);
    gConsoles = {&handheld};
    handheld.load();
    CHECK(handheld.start("3DS", text(relay.address())));
    handheld.joinCommunity(text(alice.invite(cid)));
    CHECK(until([&] {
        return handheld.communities().count(text(cid)) &&
               handheld.communities().at(text(cid)).isMember(handheld.peerId());
    }, 30000));
    const auto invite = alice.invite(cid);
    CHECK(until([&] {
        return handheld.communities().at(text(cid)).revision() ==
               text(alice.communities()[cid].revision());
    }, 30000));
    alice.stop();
    community::Node bob(bd.path());
    CHECK(bob.start("bob", {}, 0, relay.address()));
    bob.joinCommunity(invite);
    CHECK(until([&] {
        return bob.communities().contains(cid) && bob.communities()[cid].isMember(bob.peerId());
    }, 30000));
    gConsoles.clear();
}
} // namespace

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    const auto backend = qEnvironmentVariable("DISQUISITION_TEST_TLS_BACKEND");
    if (!backend.isEmpty() && !QSslSocket::setActiveBackend(backend)) {
        std::fprintf(stderr, "TLS backend unavailable: %s\n", backend.toUtf8().constData());
        return 77;
    }
    canonicalFormsMatchTheDesktop();
    relayJoinChatVoiceAndHosting();
    consoleHostsALargeHistory();
    directMemberAndSession();
    std::puts("console_client_test: ok");
    return 0;
}
