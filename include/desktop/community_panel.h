#pragma once

#include <QMap>
#include <QThreadPool>
#include <QWidget>
#include <memory>

#include "common/recent_messages.h"
#include "community/node.h"
#include "desktop/image_transfer.h"
#include "desktop/relay_audio.h"
#include "desktop/push_to_talk.h"
#include "desktop/screen_drawing_session.h"
#include "desktop/screen_capture.h"

class QTreeWidget;
class QTreeWidgetItem;
class QLineEdit;
class QPushButton;
class QLabel;
class QListWidget;
class QTextBrowser;
class ChatMedia;
class GifPicker;
class GifSearch;
class ScreenStage;
class NotificationCenter;
class MentionCompleter;

class CommunityPanel final : public QWidget {
    Q_OBJECT
public:
    explicit CommunityPanel(QWidget* parent = nullptr, const QString& directory = {});
    ~CommunityPanel() override;
    void showSettings() { settings(); }
    void reloadSavingSettings() { openHistory(); }
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
    };
    void buildUi();
    void refresh();
    void select(QTreeWidgetItem* item);
    void selectCommunity(const QString& cid);
    void connectNode();
    void settings();
    void create();
    void join();
    void acceptInvite(const QString& link);
    void invite();
    void manage();
    void sendChat();
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
    void startDrawing(const QString& peer, bool owner);
    void stopDrawing(const QString& peer);
    QStringList memberNames(const QString& cid) const;
    void share();
    void stopSharing();
    void watch(const QString& peer, bool enabled);
    QString memberName(const QString& cid, const QString& peer) const;
    void attempt(const std::function<void()>& action);
    void note(const QString& text);

    std::unique_ptr<community::Node> node_;
    QString failure_;
    bool online_ = false;
    bool refreshQueued_ = false;
    QString cid_, channel_;
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
    std::unique_ptr<chat::RecentMessages> history_;
    QLineEdit* name_ = nullptr;
    QPushButton* connect_ = nullptr;
    QTreeWidget* tree_ = nullptr;
    QTextBrowser* transcript_ = nullptr;
    QListWidget* members_ = nullptr;
    QLabel* heading_ = nullptr;
    QLabel* topic_ = nullptr;
    QLabel* status_ = nullptr;
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
    QLabel* notificationStatus_ = nullptr;
    QMap<QString, QSet<QString>> previousOnline_, previousSharing_;
    PushToTalk pushToTalk_;
    std::map<QString, std::unique_ptr<ScreenDrawingSession>> drawings_;
    std::unique_ptr<PresenterOverlay> presenterOverlay_;
    RelayAudio audio_;
    ScreenCapture capture_;
    ImageAssembler images_;
    ScreenFrameAssembler frames_;
    std::map<QString, ScreenStreamDecoder> decoders_;
    QTimer imageTimer_;
    struct Upload {
        QString cid, channel;
        quint32 transfer;
        QList<QByteArray> chunks;
        int next = 0;
    };
    QList<Upload> uploads_;
    QThreadPool imagePool_; // waits for preparation before other members are destroyed
};
