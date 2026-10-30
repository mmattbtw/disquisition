#pragma once

#include <QHash>

#include <QMap>
#include <QThreadPool>
#include <QWidget>
#include <memory>

#include "common/recent_messages.h"
#include "community/node.h"
#include "desktop/image_transfer.h"
#include "desktop/relay_audio.h"
#include "desktop/push_to_talk.h"
#include "desktop/screen_audio.h"
#include "desktop/screen_drawing_session.h"
#include "desktop/screen_capture.h"

class QAction;
class PenguinOverlay;
class QTreeWidget;
class QTreeWidgetItem;
class QLineEdit;
class QPushButton;
class QFrame;
class QLabel;
class QListWidget;
class QStackedWidget;
class QTextBrowser;
class ChatMedia;
class GifPicker;
class GifSearch;
class ScreenStage;
class StreamView;
class NotificationCenter;
class MentionCompleter;

class CommunityPanel final : public QWidget {
    Q_OBJECT
public:
    explicit CommunityPanel(QWidget* parent = nullptr, const QString& directory = {});
    ~CommunityPanel() override;
    QAction* penguinsAction() const { return penguinsAction_; }
    void showSettings() { settings(); }
    void reloadSavingSettings() { openHistory(); }
    void releaseStore();
signals:
    void activated();

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
    void dragEnterEvent(QDragEnterEvent* event) override;
    void dropEvent(QDropEvent* event) override;

private:
    struct Entry {
        chat::RecentMessage message;
        QByteArray image;
        bool undelivered = false;
        bool system = false;
    };
    void buildUi();
    void refresh();
    void select(QTreeWidgetItem* item);
    void selectCommunity(const QString& cid);
    void connectNode();
    void settings();
    void showProfile(const QString& cid, const QString& peer, const QPoint& position);
    void create();
    void join();
    void acceptInvite(const QString& link);
    void invite();
    void manage();
    void sendChat();
    void runCommand(const QString& command);
    void commandReply(const QString& title, const QString& body);
    void leaveCommunity(const QString& cid = {});
    bool deleteCommunity(const QString& cid);
    void receive(const QString& cid, const QString& channel, const QString& peer,
                 const QString& type, const QJsonObject& data);
    void append(const QString& cid, const QString& channel, Entry entry, bool persist = true);
    void render();
    void showEntry(const Entry& entry);
    void openHistory();
    void loadHistory(const QString& cid, const QString& channel);
    void prepareUpload(std::function<PreparedImage()> prepare);
    void sendImage(const PreparedImage& image, const QString& cid, const QString& channel);
    void joinVoice();
    void leaveVoice();
    void voiceState();
    void updatePushToTalk();
    void refreshPenguins();
    void publishSprite(const QString& cid, const QString& target = {});
    void startDrawing(const QString& peer, bool owner);
    void stopDrawing(const QString& peer);
    QStringList memberNames(const QString& cid) const;
    void share();
    void stopSharing();
    // Shows the screen share in place of the chat, while there is one to show.
    void openStage(bool open);
    void refreshPip();
    // Tells the call who is watching this user's share.
    void announceViewers();
    void refreshAudience();
    void placePip();
    void showFrame(const QString& peer, const QImage& image, qsizetype encodedBytes);
    void watch(const QString& peer, bool enabled);
    void setStreamVolume(const QString& peer, int percent);
    void applyVoiceSettings(const QString& peer);
    void showVoiceMemberMenu(const QString& peer, const QPoint& at);
    QString memberName(const QString& cid, const QString& peer) const;
    void attempt(const std::function<void()>& action);
    void note(const QString& text);
    void noteError(const QString& text);
    void updateJoining();
    void setSpeaking(const QString& peer, bool speaking);

    std::unique_ptr<community::Node> node_;
    QString failure_;
    bool online_ = false;
    bool refreshQueued_ = false;
    QString cid_, channel_;
    QString messageColor_;
    PenguinOverlay* penguins_ = nullptr;
    QAction* penguinsAction_ = nullptr;
    QMap<QString, QSet<QString>> spritePeers_;
    QMap<QString, QPair<QImage, QImage>> peerSprites_;
    QMap<QString, QColor> peerColors_;
    ImageAssembler spriteImages_;
    QString renderedProfiles_;
    QString voiceCid_, voiceChannel_;
    QString selectedVoiceCid_, selectedVoiceChannel_;
    QString pendingLink_;
    QMap<QString, QList<Entry>> entries_;
    QList<QPair<QString, QString>> cachedImages_;
    qint64 cachedImageBytes_ = 0;
    QMap<QString, int> unread_;
    QSet<QString> historyLoaded_;
    QSet<QString> audioPeers_;
    QSet<QString> streams_;
    QSet<QString> watching_;
    QSet<QString> watchers_;
    // Who watches each remote share, as its sharer last announced.
    QHash<QString, QSet<QString>> viewers_;
    QSet<QString> joiningCommunities_;
    QSet<QString> speaking_;
    QMap<QString, int> quietAudioFrames_;
    QMap<QString, qint64> lastAudioAt_;
    QMap<QString, qint64> lastScreenFrames_;
    QTimer streamRetry_;
    QTimer speakingTimer_;
    std::unique_ptr<chat::RecentMessages> history_;
    QLineEdit* name_ = nullptr;
    QPushButton* connect_ = nullptr;
    QTreeWidget* tree_ = nullptr;
    QTextBrowser* transcript_ = nullptr;
    QListWidget* members_ = nullptr;
    QLabel* heading_ = nullptr;
    QLabel* topic_ = nullptr;
    QLabel* status_ = nullptr;
    QLabel* connectionStatus_ = nullptr;
    QLabel* userAvatar_ = nullptr;
    QLabel* userName_ = nullptr;
    QLabel* userState_ = nullptr;
    QLabel* voiceDetail_ = nullptr;
    QFrame* composerFrame_ = nullptr;
    QStackedWidget* views_ = nullptr;
    QPushButton* stageToggle_ = nullptr;
    bool stageOpen_ = false;
    // A small live view of a share, shown over the chat while the stage is closed.
    QFrame* pip_ = nullptr;
    StreamView* pipView_ = nullptr;
    QLabel* pipName_ = nullptr;
    QString pipPeer_;
    QString pipDismissed_;
    QAction* leaveAction_ = nullptr;
    QWidget* joining_ = nullptr;
    QLabel* voiceStatus_ = nullptr;
    QWidget* voiceControls_ = nullptr;
    QPushButton* create_ = nullptr;
    QPushButton* join_ = nullptr;
    QPushButton* invite_ = nullptr;
    QPushButton* manage_ = nullptr;
    QLineEdit* composer_ = nullptr;
    QPushButton* send_ = nullptr;
    QPushButton* gif_ = nullptr;
    QPushButton* image_ = nullptr;
    QPushButton* voice_ = nullptr;
    QPushButton* mute_ = nullptr;
    QPushButton* deafen_ = nullptr;
    QPushButton* share_ = nullptr;
    ChatMedia* media_ = nullptr;
    GifPicker* gifPicker_ = nullptr;
    GifSearch* gifSearch_ = nullptr;
    ScreenStage* stage_ = nullptr;
    NotificationCenter* notifications_ = nullptr;
    MentionCompleter* mentions_ = nullptr;
    QMap<QString, QSet<QString>> previousOnline_, previousSharing_;
    PushToTalk pushToTalk_;
    std::map<QString, std::unique_ptr<ScreenDrawingSession>> drawings_;
    std::unique_ptr<PresenterOverlay> presenterOverlay_;
    RelayAudio audio_;
    ScreenCapture capture_;
    ScreenAudioPlayer screenAudio_;
    ImageAssembler images_;
    ScreenFrameAssembler frames_;
    std::map<QString, ScreenStreamDecoder> decoders_;
    QTimer imageTimer_;
    struct Upload {
        QString cid, channel;
        quint32 transfer;
        QList<QByteArray> chunks;
        int next = 0;
        QString color;
    };
    QList<Upload> uploads_;
    QThreadPool imagePool_; // waits for preparation before other members are destroyed
};
