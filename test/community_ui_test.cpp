#include <QApplication>
#include <QAction>
#include <QAbstractItemView>
#include <QBuffer>
#include <QCheckBox>
#include <QCompleter>
#include <QComboBox>
#include <QDeadlineTimer>
#include <QDesktopServices>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QHostAddress>
#include <QInputDialog>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMouseEvent>
#include <QMessageBox>
#include <QNetworkAccessManager>
#include <QNetworkProxy>
#include <QFrame>
#include <QPlainTextEdit>
#include <QPointer>
#include <QPushButton>
#include <QSettings>
#include <QSignalBlocker>
#include <QSlider>
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
#include "fake_relay_audio.h"
class ChatLinkCapture : public QObject {
    Q_OBJECT
public:
    QUrl opened;
public slots:
    void capture(const QUrl& url) { opened = url; }
};

// Stable identities keep the custom-sprite screenshot probes in separate lanes
// and positions, rather than randomly obscuring one another.
static void seedIdentity(const QString& directory, char seed) {
    CHECK(QDir().mkpath(directory));
    QFile key(directory + "/identity.key");
    CHECK(key.open(QIODevice::WriteOnly));
    CHECK(key.write(QByteArray(32, seed)) == 32);
}
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
    // A fresh identity with no saved display name waits for the user to connect.
    {
        CommunityPanel fresh(nullptr, directory.path() + "/fresh");
        QApplication::processEvents();
        CHECK(fresh.findChild<QPushButton*>("communityConnect")->text() == "Connect");
    }
    // Exercise member moderation through the actual management controls.
    {
        const auto moderationData = directory.path() + "/moderation";
        const auto kicked = community::Identity::generate();
        const auto blocked = community::Identity::generate();
        QString cid;
        {
            community::Store store(moderationData);
            auto model = community::Model::create(store.identity(), "Moderation", "Owner",
                                                  "tcp://127.0.0.1:9001");
            const auto code = model.issueInvite(store.identity());
            community::Invite invite{model.id, model.owner, code, {}};
            for (const auto* peer : {&kicked, &blocked})
                model.admit(store.identity(), community::joinIntent(*peer, invite, "Guest",
                                                                    "tcp://127.0.0.1:9002"));
            cid = model.id;
            store.save(model);
        }
        CommunityPanel moderation(nullptr, moderationData);
        auto* moderationNode = moderation.findChild<community::Node*>();
        auto* manageMembers = moderation.findChild<QPushButton*>("communityManage");
        CHECK(moderationNode && manageMembers->isEnabled());
        QTimer::singleShot(0, [&] {
            auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
            CHECK(dialog);
            auto* members = dialog->findChild<QComboBox*>("communityMember");
            auto* bans = dialog->findChild<QComboBox*>("communityBannedMember");
            auto* unban = dialog->findChild<QPushButton*>("communityUnbanMember");
            members->setCurrentIndex(members->findData(kicked.id()));
            dialog->findChild<QPushButton*>("communityKickMember")->click();
            CHECK(!moderationNode->communities()[cid].isMember(kicked.id()));
            CHECK(!moderationNode->communities()[cid].banned.contains(kicked.id()));
            CHECK(bans->count() == 0 && !unban->isEnabled());
            members->setCurrentIndex(members->findData(blocked.id()));
            dialog->findChild<QPushButton*>("communityRemoveMember")->click();
            CHECK(moderationNode->communities()[cid].banned.contains(blocked.id()));
            CHECK(bans->count() == 1 && bans->currentData().toString() == blocked.id());
            CHECK(bans->currentText().contains("Guest") && unban->isEnabled());
            unban->click();
            CHECK(!moderationNode->communities()[cid].banned.contains(blocked.id()));
            CHECK(!moderationNode->communities()[cid].isMember(blocked.id()));
            CHECK(bans->count() == 0 && !unban->isEnabled());
            CHECK(!dialog->findChild<QPushButton*>("communityKickMember")->isEnabled());
            CHECK(!dialog->findChild<QPushButton*>("communityRemoveMember")->isEnabled());
            dialog->accept();
        });
        manageMembers->click();
    }
    const auto data = directory.path() + "/community";
    app.setProperty("communityDataDirectory", data);
    QSettings().setValue("community/host", "127.0.0.1");
    QSettings().setValue("community/relay", "");
    QTcpServer reservation;
    CHECK(reservation.listen(QHostAddress::LocalHost, 0));
    seedIdentity(data, 1);
    {
        community::Store store(data);
        auto profile = store.profile();
        profile.name = "Alice";
        store.saveProfile(profile);
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
    reservation.close();
    MainWindow window;
    window.resize(1140, 780);
    window.show();
    auto* panel = window.findChild<CommunityPanel*>();
    CHECK(panel);
    CHECK(window.centralWidget() == panel);
    auto* online = panel->findChild<QPushButton*>("communityConnect");
    CHECK(online);
    // Returning clients connect from their persisted profile without a click.
    CHECK(until([&] { return online->text() == "Disconnect"; }));
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
        CHECK(relay && relay->text().isEmpty());
        auto* linkedMedia = dialog->findChild<QCheckBox*>("communityShowLinkedMedia");
        CHECK(linkedMedia && linkedMedia->isChecked());
        dialog->findChild<QLineEdit*>("userDisplayName")->setText("Unsaved name");
        dialog->reject();
    });
    panel->findChild<QPushButton*>("communitySettings")->click();
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
    if (app.arguments().contains("--chat-links-only")) {
        // URLs remain literal text, become anchors, and coexist with mentions.
        const QString linkedMessage =
            "@Alice <b>links</b> https://example.com/a?x=1&y=2.\n"
            "(http://example.org/wiki/Test_(page)) HTTPS://example.net/@everyone "
            "javascript:alert(1) file:///tmp/example";
        composer->setText(linkedMessage);
        panel->findChild<QPushButton*>("communitySend")->click();
        CHECK(transcript->toPlainText().contains(linkedMessage));
        ChatLinkCapture capture;
        QDesktopServices::setUrlHandler("http", &capture, "capture");
        QDesktopServices::setUrlHandler("https", &capture, "capture");
        for (const QString& url : {QString("https://example.com/a?x=1&y=2"),
                                   QString("http://example.org/wiki/Test_(page)"),
                                   QString("HTTPS://example.net/@everyone")}) {
            const auto anchor = transcript->document()->find(url);
            CHECK(!anchor.isNull() && anchor.charFormat().isAnchor());
            CHECK(QUrl(anchor.charFormat().anchorHref()) == QUrl(url));
            transcript->anchorClicked(QUrl(anchor.charFormat().anchorHref()));
            CHECK(capture.opened == QUrl(url));
        }
        CHECK(!transcript->document()->find("javascript:alert(1)").charFormat().isAnchor());
        CHECK(!transcript->document()->find("file:///tmp/example").charFormat().isAnchor());
        CHECK(!transcript->document()->find("<b>links</b>").charFormat().isAnchor());
        QDesktopServices::unsetUrlHandler("http");
        QDesktopServices::unsetUrlHandler("https");
        return 0;
    }
    QTimer::singleShot(0, [&] {
        auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
        CHECK(dialog);
        CHECK(!dialog->findChild<QPushButton*>("communityKickMember")->isEnabled());
        CHECK(!dialog->findChild<QPushButton*>("communityRemoveMember")->isEnabled());
        CHECK(!dialog->findChild<QPushButton*>("communityUnbanMember")->isEnabled());
        CHECK(dialog->findChild<QComboBox*>("communityBannedMember")->count() == 0);
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
        auto* link = dialog->findChild<QLineEdit*>("communityInviteLink");
        CHECK(link);
        CHECK(link->text().startsWith("disquisition://join/"));
        CHECK(dialog->grab().save("community-invite.png"));
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
    // Pressing a button in the tree gives it focus, which makes its row
    // current. That must not rebuild the tree and delete the button mid-click.
    window.activateWindow();
    CHECK(until([&] { return window.isActiveWindow(); }));
    QPointer<QPushButton> join = panel->findChild<QPushButton*>("communityJoinVoice");
    join->setFocus(Qt::MouseFocusReason);
    QApplication::processEvents();
    CHECK(join && join->isVisible());
    // Double-clicking a voice channel joins it.
    QTreeWidgetItem* lounge = nullptr;
    for (QTreeWidgetItemIterator it(tree); *it; ++it)
        if ((*it)->data(0, Qt::UserRole + 1).toString() == voiceChannel)
            lounge = *it;
    CHECK(lounge);
    const QPoint loungeCentre = tree->visualItemRect(lounge).center();
    QMouseEvent doubleClick(QEvent::MouseButtonDblClick, loungeCentre,
                            tree->viewport()->mapToGlobal(loungeCentre), Qt::LeftButton,
                            Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(tree->viewport(), &doubleClick);
    CHECK(until([&] { return panel->findChild<QPushButton*>("communityMute")->isVisible(); }));
    // Exercise presence rendering without opening microphone or screen hardware.
    CHECK(node);
    node->setVoice(cid, voiceChannel, true, true);
    CHECK(until([&] { return panel->findChild<QPushButton*>("communityMute")->isVisible(); }));
    CHECK(panel->findChild<QPushButton*>("communityDeafen")->isVisible());
    CHECK(panel->findChild<QPushButton*>("communityShare")->isVisible());
    CHECK(panel->findChild<QPushButton*>("communityCamera")->isVisible());
    // Permission denial produces an actionable dialog and resets the toggle.
    bool cameraDenied = false;
    QTimer cameraDialog;
    cameraDialog.setInterval(1);
    QObject::connect(&cameraDialog, &QTimer::timeout, [&] {
        auto* dialog = qobject_cast<QMessageBox*>(QApplication::activeModalWidget());
        if (!dialog || dialog->objectName() != "cameraPermissionDenied") return;
        cameraDenied = true;
#if defined(Q_OS_MACOS)
        bool hasSettings = false;
        for (auto* button : dialog->buttons())
            hasSettings |= button->text() == "Open Camera Settings";
        CHECK(hasSettings);
#endif
        dialog->reject();
    });
    cameraDialog.start();
    panel->findChild<QPushButton*>("communityCamera")->click();
    CHECK(until([&] { return cameraDenied; }));
    cameraDialog.stop();
    CHECK(panel->findChild<QPushButton*>("communityCamera")->text() == "Turn camera on");
    CHECK(!node->presence(cid, node->peerId()).camera);
    bool selfUnderChannel = false;
    for (QTreeWidgetItemIterator item(tree); *item; ++item)
        if ((*item)->data(0, Qt::UserRole + 2).toString() == node->peerId() && (*item)->parent() &&
            (*item)->parent()->data(0, Qt::UserRole + 1).toString() == voiceChannel) {
            selfUnderChannel = true;
            const auto activity = (*item)->data(0, Qt::UserRole + 3).toStringList();
            CHECK(activity.contains("muted") && activity.contains("deafened"));
            CHECK(tree->itemWidget(*item, 0));
            auto* row = tree->itemWidget(*item, 0);
            auto* profile = row->findChild<QPushButton*>("voiceMemberProfile");
            CHECK(profile && profile->iconSize() == QSize(20, 20));
            CHECK(profile->text() == "Alice (you)");
            CHECK((*item)->text(0).isEmpty()); // No second name painted beneath the row widget.
            CHECK((*item)->data(0, Qt::AccessibleTextRole).toString() == "Alice (you)");
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
    seedIdentity(remoteData.path(), 2);
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
    const auto channelUnread = [&](const QString& channelId) {
        for (int i = 0; i < tree->topLevelItemCount(); ++i) {
            auto* root = tree->topLevelItem(i);
            if (root->data(0, Qt::UserRole).toString() != cid) continue;
            for (int j = 0; j < root->childCount(); ++j)
                if (root->child(j)->data(0, Qt::UserRole + 1).toString() == channelId)
                    return root->child(j)->data(0, Qt::UserRole + 20).toInt();
        }
        return 0;
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
    CHECK(transcript->document()->find("Legacy color").charFormat().foreground().color() == QColor("#d7dbde"));
    CHECK(remote.sendPacket(cid, textChannel, "chat", {{"body", "Invalid color"}, {"color", "<b>bad</b>"}}) == 1);
    CHECK(until([&] { return transcript->toPlainText().contains("Invalid color"); }));
    CHECK(transcript->document()->find("Invalid color").charFormat().foreground().color() == QColor("#d7dbde"));
    const auto sender = transcript->document()->find("Bob");
    CHECK(!sender.isNull() && sender.charFormat().isAnchor());
    CHECK(sender.charFormat().anchorHref() == "profile://" + cid + '/' + remote.peerId());
    CHECK(transcript->toHtml().contains("profile-avatar:"));
    transcript->anchorClicked(QUrl(sender.charFormat().anchorHref()));
    checkProfileCard();
    // Channel unread markers and badges disappear when notifications are off.
    CHECK(remote.sendPacket(cid, musicChannel, "chat", {{"body", "unread while on"}}) == 1);
    CHECK(until([&] { return channelUnread(musicChannel) == 1; }));
    CHECK(badge.unread);
    auto settings = center->settings();
    settings.muted = true;
    center->setSettings(settings);
    CHECK(until([&] { return !badge.unread && badge.mentions == 0; }));
    CHECK(channelUnread(musicChannel) == 0);
    CHECK(remote.sendPacket(cid, musicChannel, "chat", {{"body", "quiet unread"}}) == 1);
    CHECK(until([&] {
        chat::RecentMessages history((directory.path() + "/history.db").toStdString());
        const auto saved = history.recent(cid.toStdString(), musicChannel.toStdString());
        return !saved.empty() && saved.back().body == "quiet unread";
    }));
    QApplication::processEvents();
    CHECK(channelUnread(musicChannel) == 0);
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
    CHECK(pinkMessage.charFormat().foreground().color() == QColor("#d7dbde"));
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
    CHECK(peachMessage.charFormat().foreground().color() == QColor("#d7dbde"));
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
        seedIdentity(lateData.path(), 4);
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
    auto* voiceStatus = panel->findChild<QLabel*>("communityVoiceStatus");
    CHECK(until([&] { return stage->drawing(remote.peerId()) != nullptr &&
                            remote.presence(cid, node->peerId()).voiceChannel == voiceChannel; }));
    // Profile/metadata updates must not interrupt the call.
    CHECK(voiceStatus->text().contains("Lounge"));
    auto localProfile = node->profile();
    localProfile.bio += " In voice.";
    node->setProfile(localProfile);
    CHECK(until([&] {
        return remote.communities()[cid].members[node->peerId()].bio == localProfile.bio &&
               !node->synchronizing(cid) && !remote.synchronizing(cid);
    }));
    CHECK(node->presence(cid, node->peerId()).voiceChannel == voiceChannel);
    CHECK(voiceStatus->text().contains("Lounge"));
    auto remoteProfile = remote.profile();
    remoteProfile.bio += " In voice.";
    remote.setProfile(remoteProfile);
    CHECK(until([&] {
        return node->communities()[cid].members[remote.peerId()].bio == remoteProfile.bio &&
               !node->synchronizing(cid) && !remote.synchronizing(cid);
    }));
    CHECK(remote.presence(cid, remote.peerId()).voiceChannel == voiceChannel);
    CHECK(node->presence(cid, node->peerId()).voiceChannel == voiceChannel);
    CHECK(voiceStatus->text().contains("Lounge"));
    const auto openTextChannel = [&] {
        for (QTreeWidgetItemIterator it(tree); *it; ++it)
            if ((*it)->data(0, Qt::UserRole + 1).toString() == textChannel)
                tree->setCurrentItem(*it);
        QApplication::processEvents();
    };
    // Talking peers light up their row while audio keeps arriving.
    const auto speakingRow = [&]() -> QWidget* {
        for (QTreeWidgetItemIterator it(tree); *it; ++it)
            if ((*it)->data(0, Qt::UserRole + 2).toString() == remote.peerId())
                return tree->itemWidget(*it, 0);
        return nullptr;
    };
    CHECK(until([&] { return speakingRow() != nullptr; }));
    // A silent first packet must not start a speaking highlight. A following
    // chat packet is a transport-order barrier proving the audio was processed.
    CHECK(remote.sendPacket(cid, voiceChannel, "audio",
                            {{"bytes", QString::fromLatin1(QByteArray(640, 0).toBase64())}}) == 1);
    CHECK(remote.sendPacket(cid, textChannel, "chat", {{"body", "silent audio probe"}}) == 1);
    CHECK(until([&] { return transcript->toPlainText().contains("silent audio probe"); }));
    CHECK(speakingRow() && speakingRow()->styleSheet().isEmpty());
    QByteArray loud(640, 0);
    for (int index = 0; index < loud.size(); index += 2) {
        loud[index] = char(0xe8);
        loud[index + 1] = 0x03;
    }
    CHECK(remote.sendPacket(cid, voiceChannel, "audio",
                            {{"bytes", QString::fromLatin1(loud.toBase64())}}) == 1);
    CHECK(until([&] {
        auto* row = speakingRow();
        return row && row->styleSheet().contains("background-color");
    }));
    CHECK(speakingRow()->styleSheet().contains("QWidget#voiceParticipantRow"));
    CHECK(tree->grab().save("community-voice-speaking.png"));
    {
        // The call view lights up the same person's tile.
        window.show();
        QApplication::processEvents();
        auto* call = panel->findChild<ScreenStage*>();
        CHECK(call);
        const bool opened = !call->isVisible();
        if (opened)
            panel->findChild<QPushButton*>("communityStageToggle")->click();
        QApplication::processEvents();
        CHECK(call->isVisible());
        bool glowing = false;
        for (auto* person : call->findChildren<ParticipantTile*>())
            glowing |= person->participant().id == remote.peerId() && person->speaking();
        CHECK(glowing);
        CHECK(window.grab().save("community-call-speaking.png"));
        if (opened)
            openTextChannel();
        window.hide();
    }
    for (int index = 0; index < 12; ++index)
        CHECK(remote.sendPacket(cid, voiceChannel, "audio",
                                {{"bytes", QString::fromLatin1(QByteArray(640, 0).toBase64())}}) == 1);
    CHECK(until([&] {
        auto* row = speakingRow();
        return row && row->styleSheet().isEmpty();
    }));
    // Muting a peer clears the highlight without waiting for more packets.
    CHECK(remote.sendPacket(cid, voiceChannel, "audio",
                            {{"bytes", QString::fromLatin1(loud.toBase64())}}) == 1);
    CHECK(until([&] {
        auto* row = speakingRow();
        return row && row->styleSheet().contains("background-color");
    }));
    remote.setVoice(cid, voiceChannel, true, false, true);
    CHECK(until([&] { return node->presence(cid, remote.peerId()).muted; }));
    CHECK(until([&] {
        auto* row = speakingRow();
        return row && row->styleSheet().isEmpty();
    }));
    remote.setVoice(cid, voiceChannel, false, false, true);
    CHECK(until([&] { return !node->presence(cid, remote.peerId()).muted; }));
    // Right-clicking someone in voice sets your own volume and mute for them.
    const auto useVoiceMenu = [&](int volume, bool toggleMute) {
        QTimer::singleShot(0, [&, volume, toggleMute] {
            auto* menu = qobject_cast<QMenu*>(QApplication::activePopupWidget());
            CHECK(menu);
            auto* slider = menu->findChild<QSlider*>("streamVolume");
            CHECK(slider);
            slider->setValue(volume);
            for (auto* action : menu->actions())
                if (toggleMute && action->text() == "Mute")
                    action->toggle();
            menu->close();
        });
        emit speakingRow()->customContextMenuRequested(QPoint(4, 4));
    };
    const auto mutedByYou = [&] {
        for (QTreeWidgetItemIterator it(tree); *it; ++it)
            if ((*it)->data(0, Qt::UserRole + 2).toString() == remote.peerId())
                return (*it)->data(0, Qt::UserRole + 3).toStringList().contains("muted by you");
        return false;
    };
    useVoiceMenu(40, true);
    CHECK(QSettings().value("community/voiceVolume/" + remote.peerId()).toInt() == 40);
    // The slider follows loudness as heard, so 40% plays well below 0.4.
    CHECK(testAudio::peerVolume(remote.peerId()) < 0.2);
    CHECK(testAudio::peerMuted(remote.peerId()));
    CHECK(until(mutedByYou));
    useVoiceMenu(100, true);
    CHECK(testAudio::peerVolume(remote.peerId()) == 1.0);
    CHECK(!testAudio::peerMuted(remote.peerId()));
    CHECK(until([&] { return !mutedByYou(); }));
    // Audio devices stay editable while the call is connected.
    QTimer::singleShot(0, [&] {
        auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
        CHECK(dialog);
        auto* input = dialog->findChild<QComboBox*>("communityInputDevice");
        auto* output = dialog->findChild<QComboBox*>("communityOutputDevice");
        CHECK(input && output && input->isEnabled() && output->isEnabled());
        dialog->reject();
    });
    panel->findChild<QPushButton*>("communitySettings")->click();
    CHECK(node->presence(cid, node->peerId()).voiceChannel == voiceChannel);
    CHECK(remote.presence(cid, remote.peerId()).voiceChannel == voiceChannel);
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
    // Only people watching appear under the stream: here, just this viewer.
    auto* audience = tile->findChild<QPushButton*>("streamAudience");
    CHECK(audience && until([&] { return audience->text() == "1 watching"; }));
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
    // Camera feeds subscribe automatically and play in the person's call
    // tile. Identical frame ids must go through separate assemblers and decoders.
    int cameraRequests = 0;
    const auto cameraConnection = QObject::connect(&remote, &community::Node::packetReceived,
        [&](const QString&, const QString&, const QString&, const QString& kind, const QJsonObject& data) {
            if (kind == "watch-camera" && data.value("enabled").toBool()) ++cameraRequests;
        });
    const auto cameraOf = [&](const QString& peer) {
        for (auto* person : stage->findChildren<ParticipantTile*>())
            if (person->participant().id == peer) return person->video();
        return QImage();
    };
    remote.setVoice(cid, voiceChannel, false, false, true, true);
    CHECK(until([&] { return cameraRequests > 0; }));
    CHECK(remote.sendScreenFrame(cid, voiceChannel, node->peerId(), 1, {video}, true, true));
    CHECK(until([&] { return !cameraOf(remote.peerId()).isNull(); }));
    CHECK(cameraOf(remote.peerId()).size() == source.size());
    CHECK(tile->image().pixelColor(100, 100).red() > 150);
    // Stopping the screen subscription leaves the camera subscribed.
    stage->stopWatchingRequested(remote.peerId());
    H264Encoder faceEncoder;
    QImage face(320, 180, QImage::Format_RGB32);
    face.fill(QColor("#3a78d8"));
    QByteArray faceVideo;
    bool faceKeyframe = false;
    CHECK(faceEncoder.encode(face, face.size(), 30, 0, faceVideo, faceKeyframe) && faceKeyframe);
    CHECK(remote.sendScreenFrame(cid, voiceChannel, node->peerId(), 2, {faceVideo}, true, true));
    CHECK(until([&] { return cameraOf(remote.peerId()).pixelColor(100, 100).blue() > 150; }));
    stage->watchRequested(remote.peerId());
    CHECK(remote.sendScreenFrame(cid, voiceChannel, node->peerId(), 2, {video}, true));
    CHECK(until([&] { return tile->image().pixelColor(100, 100).red() > 150; }));
    window.show();
    QApplication::processEvents();
    CHECK(window.grab().save("community-screen-share.png"));
    // Back in the chat, the watched stream keeps playing in a small window.
    auto* pip = panel->findChild<QFrame*>("streamPip");
    CHECK(pip && pip->isHidden());
    // The call view has no "back" button; a text channel leads back to chat.
    CHECK(!panel->findChild<QPushButton*>("communityStageToggle")->isVisible());
    openTextChannel();
    QApplication::processEvents();
    CHECK(pip->isVisible() && transcript->isVisible());
    CHECK(window.grab().save("community-pip.png"));
    pip->findChild<QPushButton*>("streamPipClose")->click();
    CHECK(pip->isHidden());
    window.hide();
    // Turning the camera off returns the tile to their picture and leaves the share.
    remote.setVoice(cid, voiceChannel, false, false, true, false);
    CHECK(until([&] { return stage->cameraFrame(remote.peerId()).isNull(); }));
    CHECK(cameraOf(remote.peerId()).isNull());
    CHECK(stage->drawing(remote.peerId()) != nullptr);
    QObject::disconnect(cameraConnection);
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
    // A failed selection that restores the previous device must still publish
    // the push-to-talk change applied by the same Save.
    CHECK(!node->presence(cid, node->peerId()).muted);
    testAudio::setStartResults({false, true});
    QTimer::singleShot(0, [&] {
        auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
        CHECK(dialog);
        auto* input = dialog->findChild<QComboBox*>("communityInputDevice");
        input->addItem("Unavailable microphone", "disquisition-unavailable-device");
        input->setCurrentIndex(input->count() - 1);
        dialog->findChild<QCheckBox*>("communityPushToTalk")->setChecked(true);
        dialog->findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Save)->click();
        CHECK(dialog->findChild<QLabel*>("userSettingsError")->isVisible());
        CHECK(node->presence(cid, node->peerId()).voiceChannel == quiet);
        CHECK(node->presence(cid, node->peerId()).muted);
        dialog->reject();
    });
    panel->showSettings();
    CHECK(until([&] { return remote.presence(cid, node->peerId()).muted; }));
    // With no working selected/previous/default devices, stop advertising a call.
    testAudio::setStartResults({false, false, false});
    QTimer::singleShot(0, [&] {
        auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
        CHECK(dialog);
        auto* input = dialog->findChild<QComboBox*>("communityInputDevice");
        input->addItem("Unavailable microphone", "disquisition-unavailable-device");
        input->setCurrentIndex(input->count() - 1);
        dialog->findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Save)->click();
        auto* error = dialog->findChild<QLabel*>("userSettingsError");
        CHECK(dialog->isVisible() && error->isVisible());
        CHECK(error->text().contains("could not be restored"));
        CHECK(node->presence(cid, node->peerId()).voiceChannel.isEmpty());
        dialog->reject();
    });
    panel->showSettings();
    CHECK(until([&] { return remote.presence(cid, node->peerId()).voiceChannel.isEmpty(); }));
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
    const auto visitInvite = remote.invite(visit);
    // Guarded pointers: a socket can close and be deleted while blocked.
    std::vector<QPointer<QSslSocket>> joinBlockers;
    for (auto* socket : remote.findChildren<QSslSocket*>())
        if (!socket->blockSignals(true))
            joinBlockers.emplace_back(socket);
    QTimer::singleShot(0, [&] {
        auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
        CHECK(dialog);
        dialog->findChild<QPlainTextEdit*>("communityInviteInput")->setPlainText(visitInvite);
        CHECK(dialog->grab().save("community-join.png"));
        dialog->findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Ok)->click();
    });
    panel->findChild<QPushButton*>("communityJoin")->click();
    auto* joining = panel->findChild<QWidget*>("communityJoining");
    CHECK(joining && !joining->isHidden());
    CHECK(window.grab().save("community-joining.png"));
    for (const auto& socket : joinBlockers)
        if (socket)
            socket->blockSignals(false);
    joinBlockers.clear();
    for (auto* socket : remote.findChildren<QSslSocket*>())
        if (socket->bytesAvailable())
            QMetaObject::invokeMethod(socket, "readyRead", Qt::DirectConnection);
    CHECK(until([&] { return node->communities().value(visit).isMember(node->peerId()) &&
                             !node->synchronizing(visit) && tree->topLevelItemCount() == 2; }));
    CHECK(until([&] { return joining->isHidden(); }));
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

#include "community_ui_test.moc"
