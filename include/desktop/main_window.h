#pragma once

#include <QHash>
#include <QMainWindow>
#include <QTcpServer>
#include <QTcpSocket>
#include <QThreadPool>
#include <QTimer>
#include <functional>
#include <map>
#include <memory>
#include <optional>

#include "common/protocol.h"
#include "common/recent_messages.h"
#include "desktop/image_transfer.h"
#include "desktop/push_to_talk.h"
#include "desktop/relay_audio.h"
#include "desktop/screen_capture.h"
#include "desktop/screen_stage.h"
#include "desktop/voice_engine.h"

class ChatMedia;
class GifPicker;
class GifSearch;
class MediaFetcher;
class QLabel;
class QComboBox;
class QLineEdit;
class QListWidget;
class QMediaDevices;
class QMimeData;
class QNetworkAccessManager;
class QPushButton;
class QSpinBox;
class QTextBrowser;

class MainWindow final : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override;

protected:
    bool event(QEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    struct Peer {
        QString name;
        QString host;
        quint16 chatPort = 0;
        quint16 voicePort = 0;
        QTcpSocket* socket = nullptr;
        bool voiceConnected = false;
        bool speaking = false;
        bool muted = false;
        bool deafened = false;
        bool sharingScreen = false;
    };

    void buildUi();
    void connectToServer();
    void disconnectAll();
    void sendMessage();
    void sendChat(const QString& body);
    void runCommand(const QString& command);
    void readServer();
    void handleServerMessage(const chat::Message& message);
    void addPeer(const chat::Message& message);
    void removePeer(const QString& name);
    void dialPeer(const QString& name);
    void acceptPeer();
    void attachPeerSocket(QTcpSocket* socket);
    void readPeer(QTcpSocket* socket);
    void handlePeerMessage(QTcpSocket* socket, const chat::Message& message);
    void sendFrame(QTcpSocket* socket, const chat::Message& message);
    void sendVoiceState();
    void appendChat(const QString& sender, const QString& body,
                    const QString& messageColor = "mint", bool system = false,
                    qint64 timestamp = -1);
    void appendImage(const QString& sender, const QByteArray& bytes,
                     const QString& messageColor, qint64 timestamp = -1);
    void openGifPicker(const QString& query = {});
    // Image drag and drop over the whole window.
    bool handleDrag(QEvent* event);
    bool canDropImages(const QMimeData* mime) const;
    void dropImages(const QMimeData* mime);
    void setDropOverlayVisible(bool visible);
    void prepareAndSendImage(std::function<PreparedImage()> prepare);
    void sendImage(const QByteArray& bytes);
    void receiveImageChunk(const chat::ImageChunk& chunk);
    void openSavedMessages();
    void recordChat(const chat::RecentMessage& message);
    void refreshMembers();
    void refreshAudioDevices();
    void startJoining();
    void joinVoice();
    void leaveVoice();
    void restoreVoiceState();
    void applyMicState();
    void updatePushToTalk();
    void showPreferences();
    void startTransport(bool relay);
    void handleTransportClosed();
    void retryConnection();
    void probeRelay();
    void startScreenShare();
    void stopScreenShare();
    void sendScreenFrame(quint32 frameId, const QList<QByteArray>& chunks, bool keyframe);
    void handleScreenShare(const chat::Message& message);
    void receiveScreenFrame(const chat::Message& message);
    void watchStream(const QString& name);
    void stopWatchingStream(const QString& name);
    void endRemoteStream(const QString& name);
    void endRemoteStreams();

    QLineEdit* name_ = nullptr;
    QComboBox* inputDevice_ = nullptr;
    QComboBox* outputDevice_ = nullptr;
    QPushButton* connectButton_ = nullptr;
    QPushButton* voiceButton_ = nullptr;
    QPushButton* settingsButton_ = nullptr;
    QPushButton* shareButton_ = nullptr;
    QPushButton* muteButton_ = nullptr;
    QPushButton* deafenButton_ = nullptr;
    QTextBrowser* transcript_ = nullptr;
    std::unique_ptr<chat::RecentMessages> recentMessages_;
    QListWidget* members_ = nullptr;
    QLineEdit* composer_ = nullptr;
    QPushButton* sendButton_ = nullptr;
    QPushButton* gifButton_ = nullptr;
    QLabel* dropOverlay_ = nullptr;
    QNetworkAccessManager* network_ = nullptr;
    MediaFetcher* mediaFetcher_ = nullptr;
    ChatMedia* media_ = nullptr;
    GifSearch* gifSearch_ = nullptr;
    GifPicker* gifPicker_ = nullptr;
    QLabel* connectionLabel_ = nullptr;
    QLabel* voiceLabel_ = nullptr;
    QMediaDevices* mediaDevices_ = nullptr;
    ScreenStage* stage_ = nullptr;

    QTcpSocket server_;
    QTcpSocket relayProbe_;
    QByteArray relayProbeBuffer_;
    QTcpServer peerServer_;
    QByteArray serverBuffer_;
    QHash<QTcpSocket*, QByteArray> peerBuffers_;
    QHash<QString, Peer> peers_;
    QString myName_;
    QString serverHost_ = "relay.mmatt.net";
    quint16 serverPort_ = 9000;
    QString advertiseHost_;
    bool advertiseLocal_ = false;
    quint16 preferredVoicePort_ = 5060;
    QString relayHost_ = "relay.mmatt.net";
    quint16 relayPort_ = 3333;
    bool leakMyIp_ = false;
    bool usingRelay_ = false;
    bool intentionalDisconnect_ = false;
    bool desiredConnected_ = false;
    bool transportClosedHandled_ = true;
    bool switchingToRelay_ = false;
    bool resumeVoice_ = false;
    bool resumeMuted_ = false;
    bool resumeDeafened_ = false;
    bool resumeMutedBeforeDeafen_ = false;
    bool awaitingLogin_ = false;
    quint16 activeVoicePort_ = 0;
    bool voiceWanted_ = false;
    QString messageColor_;
    bool saveMessages_ = false;
    bool savedMessagesLoaded_ = false;
    bool savingFailed_ = false;
    QString messageFile_;
    std::optional<std::int64_t> maxSavedMessages_;
    QString klipyCustomerId_;
    bool showLinkedMedia_ = true;
    bool localSpeaking_ = false;
    bool serverSpeaking_ = false;
    bool muted_ = false;
    bool deafened_ = false;
    bool mutedBeforeDeafen_ = false;
    bool pushToTalkEnabled_ = false;
    std::optional<quint32> pushToTalkKey_;
    QString pushToTalkKeyName_;
    PushToTalk pushToTalk_;
    VoiceEngine voice_;
    RelayAudio relayAudio_;
    // Screen frames travel through the server, which forwards each share
    // only to the users watching it. Your own share is decoded locally for
    // its tile on the stage.
    ScreenCapture screenCapture_;
    bool sharingScreen_ = false;
    std::unique_ptr<ScreenStreamDecoder> ownStreamDecoder_;
    bool screenFramesSeen_ = false;
    quint64 screenShareGeneration_ = 0;
    ScreenFrameAssembler screenAssembler_;
    std::map<QString, ScreenStreamDecoder> watchedStreams_;
    QTimer reconnectTimer_;
    QTimer relayProbeTimer_;
    QTimer loginTimer_;
    // Dropped images are sent as PeerImage chunks, paced so voice and
    // screen frames keep flowing, and reassembled on arrival.
    ImageAssembler imageAssembler_;
    FramePacer imagePacer_;
    // Scales and encodes dropped images off the UI thread, one at a time so
    // they arrive in the order they were dropped. Declared last so it waits
    // for running work before anything else is destroyed.
    QThreadPool imagePool_;
};
