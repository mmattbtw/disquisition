#pragma once

#include <QHash>
#include <QMainWindow>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>
#include <map>
#include <memory>
#include <optional>

#include "common/protocol.h"
#include "common/recent_messages.h"
#include "desktop/relay_audio.h"
#include "desktop/screen_capture.h"
#include "desktop/screen_stage.h"
#include "desktop/voice_engine.h"

class QLabel;
class QComboBox;
class QLineEdit;
class QListWidget;
class QMediaDevices;
class QPushButton;
class QSpinBox;
class QTextBrowser;

class MainWindow final : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override;

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
        qint64 lastAudioMs = 0;
    };

    void buildUi();
    void connectToServer();
    void disconnectAll();
    void sendMessage();
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
    void openSavedMessages();
    void recordChat(const chat::RecentMessage& message);
    void refreshMembers();
    void refreshAudioDevices();
    void startJoining();
    void joinVoice();
    void leaveVoice();
    void restoreVoiceState();
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
    QString messageColor_ = "mint";
    bool saveMessages_ = false;
    bool savedMessagesLoaded_ = false;
    bool savingFailed_ = false;
    QString messageFile_;
    std::optional<std::int64_t> maxSavedMessages_;
    bool localSpeaking_ = false;
    bool muted_ = false;
    bool deafened_ = false;
    bool mutedBeforeDeafen_ = false;
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
    QTimer speakingExpiry_;
    QTimer reconnectTimer_;
    QTimer relayProbeTimer_;
    QTimer loginTimer_;
};
