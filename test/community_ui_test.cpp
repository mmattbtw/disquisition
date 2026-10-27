#include <QApplication>
#include <QAction>
#include <QAbstractItemView>
#include <QBuffer>
#include <QCheckBox>
#include <QCompleter>
#include <QDeadlineTimer>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QHostAddress>
#include <QInputDialog>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMessageBox>
#include <QNetworkAccessManager>
#include <QNetworkProxy>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSettings>
#include <QSignalBlocker>
#include <QSslSocket>
#include <QStackedWidget>
#include <QTcpServer>
#include <QTemporaryDir>
#include <QTextBrowser>
#include <QTextCursor>
#include <QTextTable>
#include <QThread>
#include <QTimer>
#include <QTreeWidget>
#include <functional>
#include <memory>
#include <vector>

#include "check.h"
#include "community/store.h"
#include "desktop/community_panel.h"
#include "desktop/community_tree.h"
#include "desktop/gif_picker.h"
#include "desktop/main_window.h"
#include "desktop/notification_center.h"
#include "desktop/push_to_talk.h"
#include "desktop/penguin_sprites.h"
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
    QApplication::setAttribute(Qt::AA_DontUseNativeDialogs);
    QApplication app(argc, argv);
    QTemporaryDir directory;
    app.setOrganizationName("Disquisition-ui-test");
    app.setApplicationName("Disquisition-ui-test");
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, directory.path());
    auto notifications = NotificationSettings::load(QSettings());
    // Enable indicators for the badge checks; test backends post no OS notifications.
    notifications.muted = false;
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
        CHECK(dialog->windowTitle() == "User Settings");
        auto* categories = dialog->findChild<QListWidget*>("userSettingsCategories");
        auto* pages = dialog->findChild<QStackedWidget*>("userSettingsPages");
        CHECK(categories && pages && categories->count() == 5);
        CHECK(categories->geometry().right() < pages->geometry().left());
        for (int i = 0; i < categories->count(); ++i) {
            categories->setCurrentRow(i);
            CHECK(pages->currentIndex() == i);
        }
        categories->setCurrentRow(0);
        auto* relay = dialog->findChild<QLineEdit*>("communityRelayAddress");
        CHECK(relay && relay->text() == "relay.disquisition.app");
        auto* linkedMedia = dialog->findChild<QCheckBox*>("communityShowLinkedMedia");
        CHECK(linkedMedia && linkedMedia->isChecked());
        dialog->findChild<QLineEdit*>("userDisplayName")->setText("Unsaved name");
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
    auto* node = panel->findChild<community::Node*>();
    CHECK(node && node->profile().name == "Alice");
    QImage picture(160, 100, QImage::Format_RGB32);
    picture.fill(QColor("#ba7493"));
    const auto picturePath = directory.path() + "/avatar.png";
    CHECK(picture.save(picturePath));
    QTimer::singleShot(0, [&] {
        auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
        CHECK(dialog);
        QTimer::singleShot(0, [&] {
            auto* picker = qobject_cast<QFileDialog*>(QApplication::activeModalWidget());
            CHECK(picker);
            picker->selectFile(picturePath);
            static_cast<QDialog*>(picker)->accept();
        });
        dialog->findChild<QPushButton*>("userChooseAvatar")->click();
        dialog->findChild<QPlainTextEdit*>("userBio")->setPlainText(
            "Builds things. Listens to music.");
        auto* linkedMedia = dialog->findChild<QCheckBox*>("communityShowLinkedMedia");
        linkedMedia->setChecked(false);
        dialog->findChild<QCheckBox*>("communitySaveMessages")->setChecked(true);
        dialog->findChild<QLineEdit*>("communityMessageFile")->setText(directory.path() + "/history.db");
        dialog->findChild<QListWidget*>("userSettingsCategories")->setCurrentRow(4);
        QApplication::processEvents();
        CHECK(linkedMedia->isVisible());
        CHECK(dialog->grab().save("community-media-settings.png"));
        dialog->findChild<QListWidget*>("userSettingsCategories")->setCurrentRow(0);
        CHECK(dialog->grab().save("community-user-settings.png"));
        dialog->findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Save)->click();
    });
    panel->findChild<QPushButton*>("communitySettings")->click();
    CHECK(!QSettings().value("gifs/showLinkedMedia", true).toBool());
    CHECK(node->profile().bio == "Builds things. Listens to music.");
    CHECK(QImage::fromData(node->profile().avatar).size() == QSize(128, 128));
    QTimer::singleShot(0, [&] {
        auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
        CHECK(dialog);
        dialog->findChild<QPushButton*>("userRemoveAvatar")->click();
        dialog->reject();
    });
    panel->findChild<QPushButton*>("communitySettings")->click();
    CHECK(!node->profile().avatar.isEmpty()); // Cancel also cancels picture removal.
    auto* composer = panel->findChild<QLineEdit*>("communityComposer");
    CHECK(composer->isEnabled());
    auto* status = panel->findChild<QLabel*>("communityStatus");
    CHECK(status && status->isHidden());
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
            auto* row = tree->itemWidget(*item, 0);
            auto* profile = row->findChild<QPushButton*>("voiceMemberProfile");
            CHECK(profile && profile->iconSize() == QSize(28, 28));
            CHECK(row->height() >= profile->iconSize().height());
            CHECK(profile->height() >= profile->iconSize().height());
            CHECK(row->rect().contains(profile->geometry()));
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
    remote.setProfile({"Bob", "Music, games & <b>plain text</b>.", node->profile().avatar});
    CHECK(until([&] {
        return node->communities()[cid].members[remote.peerId()].bio == remote.profile().bio &&
               !node->synchronizing(cid) && !remote.synchronizing(cid);
    }));
    const auto checkProfileCard = [&] {
        auto* card = panel->findChild<QDialog*>("userProfileCard");
        CHECK(card && card->isVisible());
        CHECK(card->findChild<QLabel*>("profileDisplayName")->text() == "Bob");
        const auto* bio = card->findChild<QLabel*>("profileBio");
        CHECK(bio->text() == remote.profile().bio && bio->textFormat() == Qt::PlainText);
        CHECK(!card->findChild<QLabel*>("profileAvatar")->pixmap().isNull());
        CHECK(card->grab().save("community-profile.png"));
        card->close();
        QApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    };
    auto* members = panel->findChild<QListWidget*>("communityMembers");
    CHECK(until([&] { return members->count() == 2; }));
    for (int i = 0; i < members->count(); ++i) {
        auto* item = members->item(i);
        if (item->data(Qt::UserRole).toString() == remote.peerId()) {
            CHECK(!item->icon().isNull());
            members->itemClicked(item);
            checkProfileCard();
        }
    }
    QString textChannel;
    for (const auto& channel : node->communities()[cid].channels)
        if (channel.type == "text" && channel.name == "general") textChannel = channel.id;
    QString musicChannel;
    for (const auto& channel : node->communities()[cid].channels)
        if (channel.type == "text" && channel.name == "music") musicChannel = channel.id;
    CHECK(!textChannel.isEmpty() && !musicChannel.isEmpty());
    auto* center = window.findChild<NotificationCenter*>();
    CHECK(center && !center->settings().muted);
    UnreadBadge badge;
    QObject::connect(center, &NotificationCenter::badgeChanged, [&](const UnreadBadge& next) { badge = next; });
    const auto channelLabel = [&](const QString& channelId) {
        for (int i = 0; i < tree->topLevelItemCount(); ++i) {
            auto* root = tree->topLevelItem(i);
            if (root->data(0, Qt::UserRole).toString() != cid) continue;
            for (int j = 0; j < root->childCount(); ++j)
                if (root->child(j)->data(0, Qt::UserRole + 1).toString() == channelId)
                    return root->child(j)->text(0);
        }
        return QString();
    };
    window.hide();
    CHECK(remote.sendPacket(cid, textChannel, "chat", {{"body", "Hi @Alice"}}) == 1);
    CHECK(until([&] { return badge.mentions == 1; }));
    CHECK(badge.unread);
    CHECK(transcript->document()->find("@Alice").charFormat().background().color() == QColor("#3e4372"));
    CHECK(transcript->toHtml().contains("#453d2c"));
    const auto mentioned = transcript->document()->find("@Alice");
    auto* mentionedTable = mentioned.currentTable();
    CHECK(mentionedTable && mentionedTable->format().width().type() == QTextLength::PercentageLength &&
          mentionedTable->format().width().rawValue() == 100);
    CHECK(mentionedTable->cellAt(0, 0).format().background().color() == QColor("#453d2c"));
    CHECK(mentionedTable->cellAt(0, 1).format().background().color() == QColor("#453d2c"));
    CHECK(window.grab().save("community-mentions.png"));
    CHECK(remote.sendPacket(cid, textChannel, "chat", {{"body", "Legacy color"}}) == 1);
    CHECK(until([&] { return transcript->toPlainText().contains("Legacy color"); }));
    CHECK(transcript->document()->find("Legacy color").charFormat().foreground().color() == QColor("#c5c8c6"));
    CHECK(remote.sendPacket(cid, textChannel, "chat", {{"body", "Invalid color"}, {"color", "<b>bad</b>"}}) == 1);
    CHECK(until([&] { return transcript->toPlainText().contains("Invalid color"); }));
    CHECK(transcript->document()->find("Invalid color").charFormat().foreground().color() == QColor("#c5c8c6"));
    const auto sender = transcript->document()->find("Bob");
    CHECK(!sender.isNull() && sender.charFormat().isAnchor());
    CHECK(sender.charFormat().anchorHref() == "profile://" + cid + '/' + remote.peerId());
    CHECK(transcript->toHtml().contains("profile-avatar:"));
    transcript->anchorClicked(QUrl(sender.charFormat().anchorHref()));
    checkProfileCard();
    // Channel unread markers and badges disappear when notifications are off.
    CHECK(remote.sendPacket(cid, musicChannel, "chat", {{"body", "unread while on"}}) == 1);
    CHECK(until([&] { return channelLabel(musicChannel).contains("[1]"); }));
    CHECK(badge.unread);
    auto settings = center->settings();
    settings.muted = true;
    center->setSettings(settings);
    CHECK(until([&] { return !badge.unread && badge.mentions == 0; }));
    CHECK(!channelLabel(musicChannel).contains("["));
    CHECK(remote.sendPacket(cid, musicChannel, "chat", {{"body", "quiet unread"}}) == 1);
    CHECK(until([&] {
        chat::RecentMessages history((directory.path() + "/history.db").toStdString());
        const auto saved = history.recent(cid.toStdString(), musicChannel.toStdString());
        return !saved.empty() && saved.back().body == "quiet unread";
    }));
    QApplication::processEvents();
    CHECK(!channelLabel(musicChannel).contains("["));
    CHECK(!badge.unread && badge.mentions == 0);
    composer->setText("/dnd");
    panel->findChild<QPushButton*>("communitySend")->click();
    CHECK(!center->settings().muted && composer->text().isEmpty());
    composer->setText("/help");
    panel->findChild<QPushButton*>("communitySend")->click();
    CHECK(composer->text().isEmpty() && transcript->toPlainText().contains("/users"));
    composer->setText("/color");
    panel->findChild<QPushButton*>("communitySend")->click();
    CHECK(transcript->toPlainText().contains("pink, mint, butter, periwinkle, lilac, aqua, peach"));
    composer->setText("/users");
    panel->findChild<QPushButton*>("communitySend")->click();
    CHECK(transcript->toPlainText().contains("Alice, Bob"));
    composer->setText("/not-a-command");
    panel->findChild<QPushButton*>("communitySend")->click();
    CHECK(transcript->toPlainText().contains("Unknown command: /not-a-command"));
    QString sentColor;
    QObject::connect(&remote, &community::Node::packetReceived,
        [&](const QString&, const QString&, const QString&, const QString& type, const QJsonObject& data) {
            if (type == "chat") sentColor = data.value("color").toString();
        });
    composer->setText("/color pink");
    panel->findChild<QPushButton*>("communitySend")->click();
    CHECK(composer->text().isEmpty());
    composer->setText("A pink community message");
    panel->findChild<QPushButton*>("communitySend")->click();
    CHECK(until([&] { return sentColor == "pink"; }));
    const auto pinkMessage = transcript->document()->find("A pink community message");
    CHECK(pinkMessage.charFormat().foreground().color() == QColor("#c5c8c6"));
    CHECK(transcript->document()->find("Alice", pinkMessage.position(), QTextDocument::FindBackward)
              .charFormat().foreground().color() == QColor("#ff87af"));
    {
        chat::RecentMessages history((directory.path() + "/history.db").toStdString());
        const auto saved = history.recent(cid.toStdString(), textChannel.toStdString());
        CHECK(!saved.empty() && saved.back().body == "A pink community message");
        CHECK(saved.back().color == "pink");
    }
    CHECK(remote.sendPacket(cid, textChannel, "chat", {{"body", "A peach reply"}, {"color", "peach"}}) == 1);
    CHECK(until([&] { return transcript->toPlainText().contains("A peach reply"); }));
    const auto peachMessage = transcript->document()->find("A peach reply");
    CHECK(peachMessage.charFormat().foreground().color() == QColor("#c5c8c6"));
    CHECK(transcript->document()->find("Bob", peachMessage.position(), QTextDocument::FindBackward)
              .charFormat().foreground().color() == QColor("#ffaf87"));
    panel->findChild<QNetworkAccessManager*>()->setProxy(
        QNetworkProxy(QNetworkProxy::HttpProxy, "127.0.0.1", 1));
    QSettings().setValue("gifs/showLinkedMedia", true);
    const auto gifUrl = QUrl("https://static.klipy.com/test/communities.gif");
    panel->findChild<GifPicker*>()->chosen(gifUrl);
    CHECK(!transcript->toPlainText().contains(gifUrl.toString()));
    QSettings().setValue("gifs/showLinkedMedia", false);
    panel->findChild<GifPicker*>()->chosen(gifUrl);
    CHECK(transcript->toPlainText().contains(gifUrl.toString()));
    // Invalid commands leave the previous color intact and do not send chat.
    composer->setText("/color 250");
    panel->findChild<QPushButton*>("communitySend")->click();
    CHECK(composer->text() == "/color 250");
    composer->clear();
    // Unlock and configure the overlay, then exchange personal sprites over community links.
    auto* penguinsAction = panel->findChild<QAction*>("desktopPenguins");
    CHECK(penguinsAction && !penguinsAction->isVisible());
    composer->setText("/waddleon");
    panel->findChild<QPushButton*>("communitySend")->click();
    CHECK(penguinsAction->isVisible() && penguinsAction->isChecked());
    CHECK(QSettings().value("penguins/unlocked").toBool());
    QImage personal(152, 152, QImage::Format_ARGB32);
    personal.fill(Qt::green);
    QSettings spriteSettings;
    PenguinSprites{true, personal, personal}.save(spriteSettings);
    spriteSettings.setValue("penguins/wander", false);
    QByteArray receivedSprite;
    ImageAssembler spriteReceiver;
    QObject::connect(&remote, &community::Node::packetReceived,
        [&](const QString&, const QString&, const QString& peer, const QString& type, const QJsonObject& packet) {
            if (type != "sprite" || packet.value("count").toInt() == 0) return;
            const auto bytes = QByteArray::fromBase64(packet.value("bytes").toString().toLatin1());
            const auto result = spriteReceiver.add(peer, quint32(packet.value("transfer").toDouble()),
                packet.value("index").toInt(), packet.value("count").toInt(), bytes, 0);
            if (result) receivedSprite = *result;
        });
    QTimer::singleShot(0, [&] {
        auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
        CHECK(dialog && dialog->findChild<QCheckBox*>("communityShowPenguins")->isChecked());
        CHECK(dialog->grab().save("community-penguin-settings.png"));
        dialog->findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Save)->click();
    });
    panel->showSettings();
    CHECK(until([&] { return PenguinSprites::fromWireData(receivedSprite).complete(); }));
    CHECK(PenguinSprites::fromWireData(receivedSprite).standing.pixelColor(20, 20) == QColor(Qt::green));
    const auto overlayPixels = [](const QColor& color) {
        int count = 0;
        for (auto* widget : QApplication::topLevelWidgets()) {
            if (widget->objectName() != "penguinOverlay" || !widget->isVisible()) continue;
            const auto image = widget->grab().toImage();
            for (int y = 0; y < image.height(); ++y)
                for (int x = 0; x < image.width(); ++x) count += image.pixelColor(x, y) == color;
        }
        return count;
    };
    CHECK(overlayPixels(Qt::green) > 1000);
    personal.fill(Qt::red);
    const auto spriteChunks = splitImage(PenguinSprites{true, personal, personal}.wireData());
    for (int i = 0; i < spriteChunks.size(); ++i)
        CHECK(remote.sendPacket(cid, {}, "sprite", {{"transfer", 42}, {"index", i},
            {"count", spriteChunks.size()}, {"bytes", QString::fromLatin1(spriteChunks[i].toBase64())}}) == 1);
    CHECK(until([&] { return overlayPixels(Qt::red) > 1000; }));
    {
        QTemporaryDir lateData;
        community::Node late(lateData.path());
        QByteArray lateSprite;
        ImageAssembler lateReceiver;
        QObject::connect(&late, &community::Node::packetReceived,
            [&](const QString&, const QString&, const QString& peer, const QString& type, const QJsonObject& packet) {
                if (type != "sprite" || packet.value("count").toInt() == 0) return;
                const auto result = lateReceiver.add(peer, quint32(packet.value("transfer").toDouble()),
                    packet.value("index").toInt(), packet.value("count").toInt(),
                    QByteArray::fromBase64(packet.value("bytes").toString().toLatin1()), 0);
                if (result && peer == node->peerId()) lateSprite = *result;
            });
        CHECK(late.start("Bob", "127.0.0.1")); // Display names can coincide, identities cannot.
        late.joinCommunity(node->invite(cid));
        CHECK(until([&] { return node->online(cid).size() == 3 && late.online(cid).size() == 3 &&
                                      remote.online(cid).size() == 3 && !lateSprite.isEmpty(); }));
        CHECK(PenguinSprites::fromWireData(lateSprite).standing.pixelColor(20, 20) == QColor(Qt::green));
        personal.fill(Qt::blue);
        const auto blueChunks = splitImage(PenguinSprites{true, personal, personal}.wireData());
        for (int i = 0; i < blueChunks.size(); ++i)
            CHECK(late.sendPacket(cid, {}, "sprite", {{"transfer", 43}, {"index", i},
                {"count", blueChunks.size()}, {"bytes", QString::fromLatin1(blueChunks[i].toBase64())}}) == 2);
        CHECK(until([&] { return overlayPixels(Qt::blue) > 1000; }));
        // Characters can overlap. Removing the newcomer must reveal the original sprite.
        late.stop();
        CHECK(until([&] { return node->online(cid).size() == 2 && overlayPixels(Qt::blue) == 0; }));
        CHECK(overlayPixels(Qt::red) > 1000);
    }
    CHECK(remote.sendPacket(cid, {}, "sprite", {{"count", 0}}) == 1);
    CHECK(until([&] { return overlayPixels(Qt::red) == 0; }));
    // Hiding the overlay is local and persists without clearing personal sprites.
    penguinsAction->setChecked(false);
    CHECK(overlayPixels(Qt::green) == 0);
    CHECK(!QSettings().value("penguins/enabled", true).toBool());
    // Watch a remote community share and exchange ink through the actual panel wiring.
    node->setVoice(cid, voiceChannel);
    remote.setVoice(cid, voiceChannel, false, false, true);
    auto* stage = panel->findChild<ScreenStage*>();
    CHECK(stage);
    CHECK(until([&] { return stage->drawing(remote.peerId()) != nullptr &&
                            remote.presence(cid, node->peerId()).voiceChannel == voiceChannel; }));
    CHECK(until([&] {
        for (auto* profile : tree->findChildren<QPushButton*>("voiceMemberProfile"))
            if (profile->property("peer").toString() == remote.peerId()) {
                profile->click();
                checkProfileCard();
                return true;
            }
        return false;
    }));
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
    int watchRequests = 0;
    QObject::connect(&remote, &community::Node::packetReceived,
                     [&](const QString&, const QString&, const QString&, const QString& kind,
                         const QJsonObject& data) {
                         if (kind == "watch" && data.value("enabled").toBool())
                             ++watchRequests;
                     });
    stage->watchRequested(remote.peerId());
    CHECK(until([&] { return stage->drawing(remote.peerId())->drawingAllowed(); }));
    CHECK(stage->drawing(remote.peerId())->authorName(remote.peerId()) == "Bob");
    CHECK(stage->drawing(remote.peerId())->chatColor() == QColor("#ff87af"));
    chat::ScreenDraw stroke;
    stroke.id = stroke.stroke = 1;
    stage->drawing(remote.peerId())->segmentDrawn(stroke);
    CHECK(until([&] { return ownerInk.segments().size() == 1; }));
    CHECK(ownerInk.segments().first().author == node->peerId().toStdString());
    CHECK(until(
        [&] { return watchRequests >= 2; })); // Retry a lost watch request / missing first frame.
    StreamTile* tile = nullptr;
    for (auto* item : stage->findChildren<StreamTile*>())
        if (item->name() == remote.peerId())
            tile = item;
    CHECK(tile);
    for (const auto* label : tile->findChildren<QLabel*>())
        CHECK(!label->text().contains(remote.peerId()));
    CHECK(tile->displayName() == "Bob");
    // Encode real video, deliver it through the peer transport, and verify that
    // the panel replaces its waiting state without screen or camera access.
    H264Encoder encoder;
    QImage source(320, 180, QImage::Format_RGB32);
    source.fill(QColor("#c44967"));
    QByteArray video;
    bool keyframe = false;
    CHECK(encoder.encode(source, source.size(), 30, 0, video, keyframe));
    CHECK(!video.isEmpty() && keyframe);
    CHECK(remote.sendScreenFrame(cid, voiceChannel, node->peerId(), 1, {video}, keyframe));
    CHECK(until([&] { return !tile->image().isNull(); }));
    CHECK(tile->image().size() == source.size());
    CHECK(tile->image().pixelColor(100, 100).red() > 150);
    // A viewer joining a static screen needs an immediately requested keyframe.
    video.clear();
    keyframe = false;
    CHECK(encoder.encode(source, source.size(), 30, 33, video, keyframe, true));
    CHECK(!video.isEmpty() && keyframe);
    ScreenStreamDecoder newcomer;
    CHECK(!newcomer.decode(42, keyframe, video).isNull());
    for (auto* action : tile->findChildren<QPushButton*>())
        if (action->text() == "Fullscreen")
            action->click();
    QWidget* fullscreen = nullptr;
    for (auto* item : QApplication::topLevelWidgets())
        if (item->windowTitle() == "Bob's screen")
            fullscreen = item;
    CHECK(fullscreen);
    remote.setProfile({"Bobby", remote.profile().bio, remote.profile().avatar});
    CHECK(until([&] { return node->communities()[cid].members[remote.peerId()].name == "Bobby"; }));
    CHECK(until([&] { return tile->displayName() == "Bobby"; }));
    CHECK(fullscreen->windowTitle() == "Bobby's screen");
    for (const auto* label : tile->findChildren<QLabel*>())
        CHECK(!label->text().contains(remote.peerId()));
    fullscreen->close();
    const auto quiet = community::freshId();
    CHECK(until([&] { return !node->synchronizing(cid); }));
    node->edit(cid, "channel", {{"id", quiet}, {"name", "Quiet"}, {"type", "voice"}, {"topic", ""}});
    CHECK(until([&] { return !node->synchronizing(cid); }));
    node->edit(cid, "admin", {{"peer", remote.peerId()}, {"enabled", true}});
    CHECK(until([&] { return remote.communities()[cid].canAdmin(remote.peerId()) &&
                             !node->synchronizing(cid) && !remote.synchronizing(cid); }));
    QTreeWidgetItem* destination = nullptr;
    for (int i = 0; i < tree->topLevelItemCount(); ++i)
        if (tree->topLevelItem(i)->data(0, Qt::UserRole).toString() == cid)
            for (int j = 0; j < tree->topLevelItem(i)->childCount(); ++j)
                if (tree->topLevelItem(i)->child(j)->data(0, Qt::UserRole + 1).toString() == quiet)
                    destination = tree->topLevelItem(i)->child(j);
    CHECK(destination);
    // Dragging shows who is being moved and highlights the destination channel.
    QPixmap dragAvatar(28, 28);
    dragAvatar.fill(Qt::magenta);
    const QPixmap dragPreview = voiceDragPreview(QIcon(dragAvatar), "Bobby");
    CHECK(!dragPreview.isNull() && dragPreview.deviceIndependentSize().width() > 48);
    CHECK(dragPreview.save("community-drag-preview.png"));
    auto* communityTree = static_cast<CommunityTree*>(tree);
    const QPoint target = tree->viewport()->mapTo(tree, tree->visualItemRect(destination).center());
    const QImage plainTree = tree->grab().toImage();
    communityTree->showDropTarget(cid, quiet);
    CHECK(communityTree->dropHighlight() == quiet);
    const QImage highlightedTree = tree->grab().toImage();
    const qreal ratio = tree->devicePixelRatioF();
    const QPoint targetPixel(qRound(target.x() * ratio), qRound(target.y() * ratio));
    CHECK(highlightedTree.pixelColor(targetPixel) != plainTree.pixelColor(targetPixel));
    CHECK(tree->grab().save("community-drag-target.png"));
    communityTree->showDropTarget({}, {});
    CHECK(communityTree->dropHighlight().isEmpty());
    CHECK(static_cast<CommunityTree*>(tree)->requestVoiceMove(cid, voiceChannel, remote.peerId(), destination));
    CHECK(until([&] { return remote.presence(cid, remote.peerId()).voiceChannel == quiet; }));
    remote.moveVoice(cid, node->peerId(), quiet);
    CHECK(until([&] { return node->presence(cid, node->peerId()).voiceChannel == quiet &&
                             panel->findChild<QLabel*>("communityVoiceStatus")->text().contains("Quiet"); }));
    CHECK(!stage->drawing(remote.peerId()));
    panel->findChild<QPushButton*>("communityVoice")->click();
    CHECK(!stage->drawing(remote.peerId()));
    window.show();
    QApplication::setActiveWindow(&window);
    composer->setFocus();
    CHECK(composer->hasFocus());
    composer->setText("/color pink");
    QKeyEvent enter(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
    QApplication::sendEvent(composer, &enter);
    CHECK(composer->text().isEmpty());
    CHECK(composer->hasFocus());
    // Mention suggestions never include the local member.
    QKeyEvent at(QEvent::KeyPress, Qt::Key_At, Qt::ShiftModifier, "@");
    QApplication::sendEvent(composer, &at);
    auto* completer = panel->findChild<QCompleter*>();
    CHECK(completer);
    auto* suggestions = completer->popup();
    CHECK(suggestions && suggestions->isVisible());
    QStringList suggested;
    for (int row = 0; row < suggestions->model()->rowCount(); ++row)
        suggested.append(suggestions->model()->index(row, 0).data().toString());
    CHECK(suggested.contains("Bobby") && suggested.contains("everyone") && suggested.contains("here"));
    CHECK(!suggested.contains("Alice"));
    suggestions->hide();
    composer->clear();
    QTimer::singleShot(0, [&] {
        auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
        CHECK(dialog);
        auto* buttons = dialog->findChild<QDialogButtonBox*>();
        auto* bio = dialog->findChild<QPlainTextEdit*>("userBio");
        bio->setPlainText(QString(community::kMaxBioLength + 1, 'x'));
        buttons->button(QDialogButtonBox::Save)->click();
        CHECK(dialog->isVisible() && dialog->findChild<QLabel*>("userSettingsError")->isVisible());
        CHECK(node->profile().name == "Alice");
        bio->setPlainText("Builds things. Listens to music.");
        dialog->findChild<QLineEdit*>("userDisplayName")->setText("Alice Example");
        buttons->button(QDialogButtonBox::Save)->click();
    });
    panel->findChild<QPushButton*>("communitySettings")->click();
    CHECK(until(
        [&] { return remote.communities()[cid].members[node->peerId()].name == "Alice Example"; }));
    CHECK(transcript->toPlainText().contains("Alice Example"));
    // Transport readiness is not authority. An owner can open Manage while
    // another member has not acknowledged the latest metadata.
    {
        std::vector<std::unique_ptr<QSignalBlocker>> blockers;
        for (auto* socket : node->findChildren<QSslSocket*>())
            blockers.push_back(std::make_unique<QSignalBlocker>(socket));
        node->edit(cid, "rename", {{"name", node->communities()[cid].name}});
        CHECK(node->synchronizing(cid));
        QApplication::processEvents(QEventLoop::ExcludeSocketNotifiers);
        CHECK(manage->isEnabled() && panel->findChild<QPushButton*>("communityInvite")->isEnabled());
        CHECK(!panel->findChild<QAction*>("communityLeave")->isVisible());
        CHECK(panel->findChild<QPushButton*>("communityActions") == nullptr);
        QTimer::singleShot(0, [&] {
            auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
            CHECK(dialog && dialog->findChild<QPushButton*>("communityDelete"));
            CHECK(dialog->grab().save("community-owner-manage.png"));
            dialog->reject();
        });
        manage->click();
    }
    for (auto* socket : node->findChildren<QSslSocket*>())
        if (socket->bytesAvailable()) QMetaObject::invokeMethod(socket, "readyRead", Qt::DirectConnection);
    online->click();
    CHECK(composer->isEnabled()); // Local commands and owner controls still work offline.
    CHECK(manage->isEnabled() && node->communities()[cid].canAdmin(node->peerId()));
    // The footer stays hidden for routine activity and appears for failures.
    CHECK(status->isHidden());
    composer->setText("offline message");
    panel->findChild<QPushButton*>("communitySend")->click();
    CHECK(status->isVisible() && status->text().contains("Connect before sending"));
    composer->clear();
    composer->setText("/help");
    panel->findChild<QPushButton*>("communitySend")->click();
    CHECK(transcript->toPlainText().contains("/help") || transcript->toPlainText().contains("/users"));
    const auto confirm = [](QMessageBox::StandardButton answer) {
        QTimer::singleShot(0, [answer] {
            auto* dialog = qobject_cast<QMessageBox*>(QApplication::activeModalWidget());
            CHECK(dialog);
            dialog->button(answer)->click();
        });
    };
    QString study;
    for (const auto& model : node->communities()) if (model.name == "Study group") study = model.id;
    CHECK(!study.isEmpty());
    for (int i = 0; i < tree->topLevelItemCount(); ++i)
        if (tree->topLevelItem(i)->data(0, Qt::UserRole).toString() == study) {
            tree->setCurrentItem(tree->topLevelItem(i)); break;
        }
    CHECK(!panel->findChild<QAction*>("communityDelete"));
    const auto deleteFromManage = [&](QMessageBox::StandardButton answer) {
        QTimer::singleShot(0, [&confirm, answer] {
            auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
            CHECK(dialog && dialog->objectName() == "communityManageDialog");
            auto* remove = dialog->findChild<QPushButton*>("communityDelete");
            CHECK(remove && remove->isEnabled());
            confirm(answer);
            remove->click();
            if (answer == QMessageBox::Cancel) {
                CHECK(dialog->isVisible());
                dialog->reject();
            }
        });
        manage->click();
    };
    deleteFromManage(QMessageBox::Cancel);
    CHECK(!node->communities()[study].deleted);
    deleteFromManage(QMessageBox::Yes);
    CHECK(node->communities()[study].deleted);
    CHECK(tree->topLevelItemCount() == 1);
    online->click();
    CHECK(until([&] { return node->online(cid).size() == 2 && !node->synchronizing(cid); }));
    const auto visit = remote.createCommunity("Bobby's club");
    node->joinCommunity(remote.invite(visit));
    CHECK(until([&] { return node->communities().value(visit).isMember(node->peerId()) &&
                             !node->synchronizing(visit) && tree->topLevelItemCount() == 2; }));
    auto* leaveAction = panel->findChild<QAction*>("communityLeave");
    CHECK(leaveAction->isVisible() && leaveAction->isEnabled());
    CHECK(!manage->isEnabled() && !node->communities()[visit].canAdmin(node->peerId()));
    const auto leaveFromSidebar = [&](QTreeWidgetItem* server) {
        bool offered = false;
        QTimer::singleShot(0, [&] {
            auto* menu = qobject_cast<QMenu*>(QApplication::activePopupWidget());
            CHECK(menu);
            QAction* action = nullptr;
            for (auto* candidate : menu->actions())
                if (candidate->objectName() == "communityLeave")
                    action = candidate;
            CHECK(action && action->isVisible() && action->isEnabled());
            offered = true;
            confirm(QMessageBox::Yes);
            action->trigger();
        });
        QMetaObject::invokeMethod(tree, "customContextMenuRequested", Qt::DirectConnection,
                                  Q_ARG(QPoint, tree->visualItemRect(server).center()));
        CHECK(offered);
    };
    {
        std::vector<std::unique_ptr<QSignalBlocker>> blockers;
        for (auto* socket : node->findChildren<QSslSocket*>())
            blockers.push_back(std::make_unique<QSignalBlocker>(socket));
        auto profile = node->profile();
        profile.bio += "!";
        node->setProfile(profile);
        CHECK(node->synchronizing(visit));
        QApplication::processEvents(QEventLoop::ExcludeSocketNotifiers);
        CHECK(leaveAction->isEnabled());
        QTreeWidgetItem* visitRoot = nullptr;
        for (int i = 0; i < tree->topLevelItemCount(); ++i)
            if (tree->topLevelItem(i)->data(0, Qt::UserRole).toString() == visit)
                visitRoot = tree->topLevelItem(i);
        CHECK(visitRoot);
        leaveFromSidebar(visitRoot);
        CHECK(!node->communities().value(visit).isMember(node->peerId()));
    }
    for (auto* socket : node->findChildren<QSslSocket*>())
        if (socket->bytesAvailable()) QMetaObject::invokeMethod(socket, "readyRead", Qt::DirectConnection);
    CHECK(until([&] { return !node->communities().value(visit).isMember(node->peerId()) &&
                             !remote.communities().value(visit).isMember(node->peerId()); }));
    CHECK(tree->topLevelItemCount() == 1);
    online->click();
    std::puts("community_ui_test: ok");
}
