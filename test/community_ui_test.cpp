#include <QApplication>
#include <QDeadlineTimer>
#include <QDialog>
#include <QCheckBox>
#include <QKeyEvent>
#include <QHostAddress>
#include <QInputDialog>
#include <QLineEdit>
#include <QListWidget>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSettings>
#include <QStackedWidget>
#include <QTcpServer>
#include <QTemporaryDir>
#include <QTextBrowser>
#include <QThread>
#include <QTimer>
#include <QTreeWidget>
#include <functional>

#include "check.h"
#include "community/store.h"
#include "desktop/community_panel.h"
#include "desktop/main_window.h"
#include "desktop/notification_center.h"
#include "desktop/push_to_talk.h"
#include "desktop/screen_stage.h"
static bool until(const std::function<bool()>& test) {
    QDeadlineTimer deadline(6000);
    while (!deadline.hasExpired()) {
        QApplication::processEvents(QEventLoop::AllEvents, 10);
        if (test())
            return true;
        QThread::msleep(1);
    }
    return test();
}
int main(int argc, char** argv) {
    QApplication app(argc, argv);
    QTemporaryDir directory;
    app.setOrganizationName("Disquisition-ui-test");
    app.setApplicationName("Disquisition-ui-test");
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, directory.path());
    auto notifications = NotificationSettings::load(QSettings());
    notifications.muted = true; // Exercise badges without posting OS notifications.
    QSettings notificationSettings;
    notifications.save(notificationSettings);
    const auto data = directory.path() + "/community";
    app.setProperty("communityDataDirectory", data);
    QSettings().setValue("community/name", "Alice");
    QSettings().setValue("community/host", "127.0.0.1");
    QTcpServer reservation;
    CHECK(reservation.listen(QHostAddress::LocalHost, 0));
    {
        community::Store store(data);
        auto model = community::Model::create(store.identity(), "After hours", "Alice",
                                              "tcp://127.0.0.1:" +
                                                  QString::number(reservation.serverPort()));
        model.change(store.identity(), "channel",
                     {{"id", community::freshId()},
                      {"name", "music"},
                      {"type", "text"},
                      {"topic", "What are you listening to?"}});
        store.save(model);
    }
    MainWindow window;
    window.resize(1140, 780);
    window.show();
    auto* panel = window.findChild<CommunityPanel*>();
    CHECK(panel);
    CHECK(window.centralWidget() == panel);
    CHECK(!panel->findChild<QPushButton*>("communityMute")->isVisible());
    CHECK(!panel->findChild<QPushButton*>("communityDeafen")->isVisible());
    CHECK(!panel->findChild<QPushButton*>("communityShare")->isVisible());
    QTimer::singleShot(0, [&] {
        auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
        CHECK(dialog);
        auto* relay = dialog->findChild<QLineEdit*>("communityRelayAddress");
        CHECK(relay && relay->text() == "relay.disquisition.app");
        dialog->reject();
    });
    panel->findChild<QPushButton*>("communitySettings")->click();
    // This isolated fixture uses direct loopback connections.
    QSettings().setValue("community/relay", "");
    auto* online = panel->findChild<QPushButton*>("communityConnect");
    CHECK(online);
    reservation.close();
    online->click();
    auto* manage = panel->findChild<QPushButton*>("communityManage");
    CHECK(until([&] { return manage->isEnabled(); }));
    auto* composer = panel->findChild<QLineEdit*>("communityComposer");
    CHECK(composer->isEnabled());
    composer->setText("Hello from our first community.");
    panel->findChild<QPushButton*>("communitySend")->click();
    auto* transcript = panel->findChild<QTextBrowser*>("communityTranscript");
    CHECK(transcript->toPlainText().contains("not delivered"));
    QTimer::singleShot(0, [&] {
        auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
        CHECK(dialog);
        auto* categories = dialog->findChild<QListWidget*>("communityManageCategories");
        auto* pages = dialog->findChild<QStackedWidget*>("communityManagePages");
        CHECK(categories && pages && categories->count() == 5);
        CHECK(categories->geometry().right() < pages->geometry().left());
        for (int i = 0; i < categories->count(); ++i) {
            categories->setCurrentRow(i);
            CHECK(pages->currentIndex() == i);
        }
        categories->setCurrentRow(0);
        CHECK(dialog->grab().save("community-manage.png"));
        for (auto* edit : dialog->findChildren<QLineEdit*>())
            if (edit->text() == "After hours")
                edit->setText("After hours club");
        dialog->findChild<QPushButton*>("communitySaveName")->click();
        dialog->accept();
    });
    manage->click();
    auto* tree = panel->findChild<QTreeWidget*>("communityTree");
    CHECK(until([&] { return tree->topLevelItem(0)->text(0) == "After hours club"; }));
    CHECK(until([&] { return panel->findChild<QPushButton*>("communityInvite")->isEnabled(); }));
    QTimer::singleShot(0, [&] {
        auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
        CHECK(dialog);
        auto* link = dialog->findChild<QPlainTextEdit*>();
        CHECK(link);
        CHECK(link->toPlainText().startsWith("disquisition://join/"));
        dialog->accept();
    });
    panel->findChild<QPushButton*>("communityInvite")->click();
    QTimer::singleShot(0, [&] {
        auto* dialog = qobject_cast<QInputDialog*>(QApplication::activeModalWidget());
        CHECK(dialog);
        dialog->setTextValue("Study group");
        dialog->accept();
    });
    panel->findChild<QPushButton*>("communityCreate")->click();
    CHECK(until([&] { return tree->topLevelItemCount() == 2; }));
    CHECK(!transcript->toPlainText().contains("Hello from our first community."));
    // Return to the populated community for a visual check.
    QTreeWidgetItem* root = nullptr;
    for (int i = 0; i < tree->topLevelItemCount(); ++i)
        if (tree->topLevelItem(i)->text(0) == "After hours club")
            root = tree->topLevelItem(i);
    CHECK(root);
    for (int i = 0; i < root->childCount(); ++i)
        if (root->child(i)->text(0).contains("general")) {
            tree->setCurrentItem(root->child(i));
            break;
        }
    QApplication::processEvents();
    // Selecting voice keeps the text channel open and exposes an inline join.
    root = nullptr; // Selection rebuilds the tree; reacquire its current items.
    for (int i = 0; i < tree->topLevelItemCount(); ++i)
        if (tree->topLevelItem(i)->text(0) == "After hours club")
            root = tree->topLevelItem(i);
    CHECK(root);
    QTreeWidgetItem* voiceItem = nullptr;
    for (int i = 0; i < root->childCount(); ++i)
        if (root->child(i)->text(0).contains("Lounge"))
            voiceItem = root->child(i);
    CHECK(voiceItem);
    const auto voiceChannel = voiceItem->data(0, Qt::UserRole + 1).toString();
    const auto cid = root->data(0, Qt::UserRole).toString();
    const auto chatBefore = transcript->toPlainText();
    tree->setCurrentItem(voiceItem);
    CHECK(transcript->toPlainText() == chatBefore && composer->isEnabled());
    CHECK(panel->findChild<QPushButton*>("communityJoinVoice")->isVisible());
    CHECK(!panel->findChild<QPushButton*>("communityMute")->isVisible());
    CHECK(window.grab().save("community-ui.png"));
    // Exercise presence rendering without opening microphone or screen hardware.
    auto* node = panel->findChild<community::Node*>();
    CHECK(node);
    node->setVoice(cid, voiceChannel, true, true);
    CHECK(until([&] { return panel->findChild<QPushButton*>("communityMute")->isVisible(); }));
    CHECK(panel->findChild<QPushButton*>("communityDeafen")->isVisible());
    CHECK(panel->findChild<QPushButton*>("communityShare")->isVisible());
    bool selfUnderChannel = false;
    for (QTreeWidgetItemIterator item(tree); *item; ++item)
        if ((*item)->text(0) == "Alice (you)" && (*item)->parent() &&
            (*item)->parent()->data(0, Qt::UserRole + 1).toString() == voiceChannel) {
            selfUnderChannel = true;
            const auto activity = (*item)->data(0, Qt::UserRole + 3).toStringList();
            CHECK(activity.contains("muted") && activity.contains("deafened"));
            CHECK(tree->itemWidget(*item, 0));
        }
    CHECK(selfUnderChannel);
    CHECK(window.grab().save("community-voice.png"));
    // Exercise the real hotkey event filter and community presence without audio hardware.
    QSettings().setValue("voice/pushToTalk", true);
    QSettings().setValue("voice/pushToTalkKey", 42);
    auto* ptt = panel->findChild<PushToTalk*>();
    CHECK(ptt);
    ptt->start(42, false);
    auto* mute = panel->findChild<QPushButton*>("communityMute");
    auto* deafen = panel->findChild<QPushButton*>("communityDeafen");
    deafen->setChecked(false);
    mute->setChecked(false);
    CHECK(node->presence(cid, node->peerId()).muted);
    QKeyEvent down(QEvent::KeyPress, Qt::Key_F8, Qt::NoModifier, 42, 42, 0);
    QKeyEvent up(QEvent::KeyRelease, Qt::Key_F8, Qt::NoModifier, 42, 42, 0);
    QApplication::sendEvent(composer, &down);
    CHECK(ptt->held() && !node->presence(cid, node->peerId()).muted);
    mute->setChecked(true);
    CHECK(node->presence(cid, node->peerId()).muted);
    QApplication::sendEvent(composer, &up);
    QApplication::sendEvent(composer, &down);
    CHECK(node->presence(cid, node->peerId()).muted); // Holding the key cannot undo manual mute.
    mute->setChecked(false);
    deafen->setChecked(true);
    CHECK(node->presence(cid, node->peerId()).muted && node->presence(cid, node->peerId()).deafened);
    deafen->setChecked(false);
    CHECK(!node->presence(cid, node->peerId()).muted);
    QApplication::sendEvent(composer, &up);
    CHECK(node->presence(cid, node->peerId()).muted && !mute->isChecked());
    QApplication::sendEvent(composer, &down);
    panel->findChild<QPushButton*>("communityVoice")->click();
    CHECK(!panel->findChild<QPushButton*>("communityMute")->isVisible());
    CHECK(!panel->findChild<QPushButton*>("communityDeafen")->isVisible());
    CHECK(!panel->findChild<QPushButton*>("communityShare")->isVisible());
    CHECK(!ptt->held());
    QSettings().setValue("voice/pushToTalk", false);
    // Incoming channel messages pass through mention detection and update the app badge.
    QTemporaryDir remoteData;
    community::Node remote(remoteData.path());
    CHECK(remote.start("Bob", "127.0.0.1"));
    remote.joinCommunity(node->invite(cid));
    CHECK(until([&] { return node->online(cid).size() == 2 && remote.online(cid).size() == 2; }));
    QString textChannel;
    for (const auto& channel : node->communities()[cid].channels)
        if (channel.type == "text" && channel.name == "general") textChannel = channel.id;
    auto* center = window.findChild<NotificationCenter*>();
    CHECK(center && center->settings().muted);
    UnreadBadge badge;
    QObject::connect(center, &NotificationCenter::badgeChanged, [&](const UnreadBadge& next) { badge = next; });
    window.hide();
    CHECK(remote.sendPacket(cid, textChannel, "chat", {{"body", "Hi @Alice"}}) == 1);
    CHECK(until([&] { return badge.mentions == 1; }));
    CHECK(badge.unread);
    CHECK(transcript->toHtml().contains("#a8e6df"));
    composer->setText("/dnd");
    panel->findChild<QPushButton*>("communitySend")->click();
    CHECK(!center->settings().muted && composer->text().isEmpty());
    // Watch a remote community share and exchange ink through the actual panel wiring.
    node->setVoice(cid, voiceChannel);
    remote.setVoice(cid, voiceChannel, false, false, true);
    auto* stage = panel->findChild<ScreenStage*>();
    CHECK(stage);
    CHECK(until([&] { return stage->drawing(remote.peerId()) != nullptr &&
                            remote.presence(cid, node->peerId()).voiceChannel == voiceChannel; }));
    ScreenDrawing ownerInk;
    ScreenDrawingSession owner(remote.peerId(), remote.peerId(), &ownerInk, true);
    QObject::connect(&owner, &ScreenDrawingSession::sendToPeer, &remote,
        [&](const QString& target, const chat::Message& message) {
            remote.sendPacket(cid, voiceChannel, "drawing", {{"target", target},
                {"bytes", QString::fromLatin1(QByteArray::fromStdString(chat::encode(message)).toBase64())}});
        });
    QObject::connect(&remote, &community::Node::packetReceived, &owner,
        [&](const QString&, const QString&, const QString& sender, const QString& type, const QJsonObject& data) {
            if (type != "drawing") return;
            auto bytes = QByteArray::fromBase64(data["bytes"].toString().toLatin1()).toStdString();
            chat::Message message;
            CHECK(chat::decode(bytes, message) == chat::DecodeStatus::Ok && bytes.empty());
            owner.receive(sender, message);
        });
    ownerInk.permissionRequested(true);
    stage->watchRequested(remote.peerId());
    CHECK(until([&] { return stage->drawing(remote.peerId())->drawingAllowed(); }));
    CHECK(stage->drawing(remote.peerId())->authorName(remote.peerId()) == "Bob");
    chat::ScreenDraw stroke;
    stroke.id = stroke.stroke = 1;
    stage->drawing(remote.peerId())->segmentDrawn(stroke);
    CHECK(until([&] { return ownerInk.segments().size() == 1; }));
    CHECK(ownerInk.segments().first().author == node->peerId().toStdString());
    panel->findChild<QPushButton*>("communityVoice")->click();
    CHECK(!stage->drawing(remote.peerId()));
    online->click();
    CHECK(composer->isEnabled() == false);
    std::puts("community_ui_test: ok");
}
