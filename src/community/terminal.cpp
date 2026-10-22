#include "community/terminal.h"

#include <unistd.h>

#include <QCommandLineParser>
#include <QCoreApplication>
#include <QDateTime>
#include <QSocketNotifier>
#include <QStandardPaths>
#include <cstdio>
#include <functional>

#include "common/recent_messages.h"
#include "community/node.h"

namespace community {
namespace {
void print(const QString& message) {
    std::printf("%s\n", message.toUtf8().constData());
    std::fflush(stdout);
}
constexpr auto help = R"(Commands:
  /communities                 list saved communities
  /create NAME                 create a community owned by this client
  /join INVITE_LINK            join; retry if every host is offline
  /use ID_OR_NAME               select a community
  /channels                    list channels
  /channel ID_OR_NAME           select a text channel
  /channel-add text|voice NAME  create a channel
  /channel-delete              delete the selected channel
  /topic TEXT                  set the selected channel's topic
  /rename NAME                 rename the community
  /invite                      issue an invite link
  /invites                     list invite IDs and revocations
  /revoke INVITE_ID             revoke an invite
  /members                     list members, roles, and endpoints
  /admin PEER_ID on|off         owner assigns or removes an admin
  /kick PEER_ID                 remove and block a member
  /priority PEER_ID,...         set preferred host order
  /history                     show this device's saved channel messages
  /help                        show commands
  /quit                        disconnect
Other input sends a live message to the selected text channel.
Voice and screen sharing are available in the desktop Communities tab.)";
QString resolve(const QStringList& ids, const QString& value) {
    QString match;
    for (const auto& id : ids)
        if (id == value || id.startsWith(value)) {
            if (!match.isEmpty())
                throw std::runtime_error("ID is ambiguous");
            match = id;
        }
    if (match.isEmpty())
        throw std::runtime_error("unknown ID");
    return match;
}
} // namespace

int runTerminal(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    app.setOrganizationName("Disquisition");
    app.setApplicationName("Disquisition");
    QCommandLineParser parser;
    parser.setApplicationDescription(
        "Decentralized community client. No discovery server required.");
    parser.addHelpOption();
    parser.addOption({{"n", "name"}, "Display name", "name"});
    parser.addOption(
        {"data", "Identity and community metadata directory", "directory",
         QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + "/communities"});
    parser.addOption({"relay", "Community relay endpoint, e.g. relay://host:3334", "endpoint"});
    parser.addOption({"advertise", "Direct hostname/IP peers should dial", "host"});
    parser.addOption({"p2p-port", "Direct listen port; 0 selects a free port", "port", "0"});
    parser.addOption({"create", "Create a community on startup", "name"});
    parser.addOption({"join", "Join an invite on startup", "link"});
    parser.addOption({"save-messages", "Continuously save this device's chat to SQLite", "file"});
    parser.addOption({"max-saved-messages", "Maximum saved messages in that file", "count"});
    parser.process(app);
    try {
        bool validPort = false;
        const auto port = parser.value("p2p-port").toUInt(&validPort);
        if (!validPort || port > 65535)
            throw std::runtime_error("invalid p2p port");
        std::unique_ptr<chat::RecentMessages> history;
        if (parser.isSet("save-messages")) {
            std::optional<std::int64_t> maximum;
            if (parser.isSet("max-saved-messages")) {
                bool valid = false;
                const auto value = parser.value("max-saved-messages").toLongLong(&valid);
                if (!valid || value <= 0)
                    throw std::runtime_error("saved message maximum must be positive");
                maximum = value;
            }
            history = std::make_unique<chat::RecentMessages>(
                parser.value("save-messages").toStdString(), maximum);
        }
        else if (parser.isSet("max-saved-messages"))
            throw std::runtime_error("--max-saved-messages requires --save-messages");
        Node node(parser.value("data"));
        QString cid, channel, lastStatus;
        const auto select = [&](const QString& id) {
            cid = id;
            channel.clear();
            for (const auto& item : node.communities()[cid].channels)
                if (item.type == "text") {
                    channel = item.id;
                    break;
                }
            print("Selected " + node.communities()[cid].name + " / " +
                  node.communities()[cid].channels.value(channel).name);
        };
        QObject::connect(&node, &Node::errorOccurred,
                         [](const QString& message) { print("Connection: " + message); });
        QObject::connect(&node, &Node::joinRejected,
                         [](const QString&, const QString& reason) { print(reason); });
        QObject::connect(&node, &Node::joined, [&](const QString& id) { select(id); });
        QObject::connect(&node, &Node::changed, [&] {
            if (cid.isEmpty() && !node.communities().isEmpty())
                select(node.communities().firstKey());
            if (cid.isEmpty())
                return;
            const auto model = node.communities()[cid];
            const auto status = model.name + ": " + QString::number(node.online(cid).size()) +
                                " online, host " + model.members.value(node.host(cid)).name;
            if (status != lastStatus) {
                print(status);
                lastStatus = status;
            }
        });
        QObject::connect(
            &node, &Node::packetReceived,
            [&](const QString& id, const QString& ch, const QString& peer, const QString& type,
                const QJsonObject& data) {
                if (type != "chat")
                    return;
                const auto model = node.communities()[id];
                const auto sender = model.members.value(peer).name;
                print("[" + model.name + "/#" + model.channels.value(ch).name + "] " + sender +
                      ": " + data.value("body").toString());
                if (history)
                    try {
                        history->append({qint64(data.value("timestamp").toDouble()) / 1000,
                                         sender.toStdString(),
                                         data.value("body").toString().toStdString(), "mint",
                                         id.toStdString(), ch.toStdString(),
                                         data.value("id").toString().toStdString()});
                    }
                    catch (const std::exception& error) {
                        print("Could not save message: " + QString::fromUtf8(error.what()));
                    }
            });
        if (!node.start(parser.value("name"), parser.value("advertise"), quint16(port),
                        parser.value("relay")))
            return 1;
        print("Peer " + node.peerId() + "\nEndpoint " + node.endpoint() +
              "\nType /help for commands.");
        if (parser.isSet("create"))
            select(node.createCommunity(parser.value("create")));
        if (parser.isSet("join"))
            node.joinCommunity(parser.value("join"));

        const auto command = [&](const QString& line) {
            try {
                if (line.trimmed().isEmpty())
                    return;
                if (!line.startsWith('/')) {
                    if (cid.isEmpty() || channel.isEmpty())
                        throw std::runtime_error("select a community and text channel first");
                    const auto id = freshId();
                    const auto stamp = QDateTime::currentSecsSinceEpoch();
                    const auto count = node.sendPacket(
                        cid, channel, "chat",
                        {{"id", id}, {"body", line}, {"timestamp", double(stamp * 1000)}});
                    print("you: " + line + (count ? "" : " [not delivered: no peers connected]"));
                    if (history)
                        history->append({stamp, parser.value("name").toStdString(),
                                         line.toStdString(), "mint", cid.toStdString(),
                                         channel.toStdString(), id.toStdString()});
                    return;
                }
                const int separator = line.indexOf(' ');
                const auto verb = separator < 0 ? line : line.left(separator);
                const auto argument = separator < 0 ? QString{} : line.mid(separator + 1).trimmed();
                if (verb == "/quit") {
                    app.quit();
                    return;
                }
                if (verb == "/help") {
                    print(help);
                    return;
                }
                if (verb == "/create") {
                    select(node.createCommunity(argument));
                    return;
                }
                if (verb == "/join") {
                    node.joinCommunity(argument);
                    return;
                }
                if (verb == "/communities") {
                    for (const auto& model : node.communities())
                        print(model.id + "  " + model.name);
                    return;
                }
                if (verb == "/use") {
                    QString selected;
                    for (const auto& model : node.communities())
                        if (model.name == argument) {
                            if (!selected.isEmpty())
                                throw std::runtime_error("community name is ambiguous; use its ID");
                            selected = model.id;
                        }
                    select(selected.isEmpty() ? resolve(node.communities().keys(), argument)
                                              : selected);
                    return;
                }
                if (!node.communities().contains(cid))
                    throw std::runtime_error("select a community first");
                const auto model = node.communities()[cid];
                if (verb == "/channels") {
                    for (const auto& item : model.channels)
                        print(item.id + "  " + item.type + "  " + item.name + "  " + item.topic);
                }
                else if (verb == "/channel") {
                    QString selected;
                    for (const auto& item : model.channels)
                        if (item.name == argument && item.type == "text")
                            selected = item.id;
                    channel = selected.isEmpty() ? resolve(model.channels.keys(), argument)
                                                 : selected;
                    if (model.channels[channel].type != "text") {
                        channel.clear();
                        throw std::runtime_error(
                            "select a text channel; voice uses the desktop client");
                    }
                }
                else if (verb == "/channel-add") {
                    const int index = argument.indexOf(' ');
                    if (index < 0)
                        throw std::runtime_error("use /channel-add text|voice NAME");
                    node.edit(cid, "channel",
                              {{"id", freshId()},
                               {"type", argument.left(index)},
                               {"name", argument.mid(index + 1).trimmed()},
                               {"topic", ""}});
                }
                else if (verb == "/channel-delete")
                    node.edit(cid, "delete-channel", {{"id", channel}});
                else if (verb == "/topic") {
                    auto item = model.channels.value(channel);
                    if (item.id.isEmpty())
                        throw std::runtime_error("select a channel first");
                    node.edit(cid, "channel",
                              {{"id", item.id},
                               {"name", item.name},
                               {"type", item.type},
                               {"topic", argument}});
                }
                else if (verb == "/rename")
                    node.edit(cid, "rename", {{"name", argument}});
                else if (verb == "/invite")
                    print(node.invite(cid));
                else if (verb == "/invites") {
                    for (const auto& id : model.inviteIds())
                        print(id + (model.revokedInvites.contains(id) ? " revoked" : " active"));
                }
                else if (verb == "/revoke")
                    node.edit(cid, "revoke", {{"hash", resolve(model.inviteIds(), argument)}});
                else if (verb == "/members") {
                    for (const auto& peer : model.members)
                        print(peer.id + "  " + peer.name +
                              (peer.id == model.owner ? " owner"
                               : peer.admin           ? " admin"
                                                      : " member") +
                              (node.online(cid).contains(peer.id) ? " online " : " offline ") +
                              peer.endpoint);
                }
                else if (verb == "/admin") {
                    const auto parts = argument.split(' ', Qt::SkipEmptyParts);
                    if (parts.size() != 2 || (parts[1] != "on" && parts[1] != "off"))
                        throw std::runtime_error("use /admin PEER_ID on|off");
                    node.edit(cid, "admin",
                              {{"peer", resolve(model.members.keys(), parts[0])},
                               {"enabled", parts[1] == "on"}});
                }
                else if (verb == "/kick")
                    node.edit(cid, "kick", {{"peer", resolve(model.members.keys(), argument)}});
                else if (verb == "/priority") {
                    QJsonArray peers;
                    for (const auto& id : argument.split(',', Qt::SkipEmptyParts))
                        peers.append(resolve(model.members.keys(), id.trimmed()));
                    node.edit(cid, "priority", {{"peers", peers}});
                }
                else if (verb == "/history") {
                    if (!history)
                        throw std::runtime_error(
                            "start with --save-messages FILE to keep local history");
                    for (const auto& message :
                         history->recent(cid.toStdString(), channel.toStdString()))
                        print(QString::fromStdString(message.sender + ": " + message.body));
                }
                else
                    throw std::runtime_error("unknown command; use /help");
            }
            catch (const std::exception& error) {
                print("Error: " + QString::fromUtf8(error.what()));
            }
        };
        QSocketNotifier input(STDIN_FILENO, QSocketNotifier::Read);
        QByteArray buffer;
        QObject::connect(&input, &QSocketNotifier::activated, [&] {
            char bytes[4096];
            const auto size = ::read(STDIN_FILENO, bytes, sizeof(bytes));
            if (size <= 0) {
                input.setEnabled(false);
                app.quit();
                return;
            }
            buffer.append(bytes, size);
            if (buffer.size() > 16384) {
                buffer.clear();
                print("Input line too long.");
                return;
            }
            qsizetype end;
            while ((end = buffer.indexOf('\n')) >= 0) {
                const auto line = buffer.left(end);
                buffer.remove(0, end + 1);
                command(QString::fromUtf8(line).trimmed());
            }
        });
        return app.exec();
    }
    catch (const std::exception& error) {
        print("Error: " + QString::fromUtf8(error.what()));
        return 1;
    }
}
} // namespace community
