#pragma once

#include <QHash>
#include <QMainWindow>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>

#include "common/protocol.h"
#include "common/recent_messages.h"
#include "desktop/voice_engine.h"
#include "desktop/relay_audio.h"

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
                    const QString& messageColor = "mint", bool system = false);
    void refreshMembers();
    void refreshAudioDevices();
    void startJoining();
    void joinVoice();
    void leaveVoice();
    void restoreVoiceState();
    void showPreferences();
    void saveRecentMessages();
    void startTransport(bool relay);
    void handleTransportClosed();
    void retryConnection();
    void probeRelay();

    QLineEdit* name_ = nullptr;
    QComboBox* inputDevice_ = nullptr;
    QComboBox* outputDevice_ = nullptr;
    QPushButton* connectButton_ = nullptr;
    QPushButton* voiceButton_ = nullptr;
    QPushButton* settingsButton_ = nullptr;
    QPushButton* muteButton_ = nullptr;
    QPushButton* deafenButton_ = nullptr;
    QTextBrowser* transcript_ = nullptr;
    chat::RecentMessages recentMessages_;
    QListWidget* members_ = nullptr;
    QLineEdit* composer_ = nullptr;
    QPushButton* sendButton_ = nullptr;
    QLabel* connectionLabel_ = nullptr;
    QLabel* voiceLabel_ = nullptr;
    QMediaDevices* mediaDevices_ = nullptr;

    QTcpSocket server_;
    QTcpSocket relayProbe_;
    QByteArray relayProbeBuffer_;
    QTcpServer peerServer_;
    QByteArray serverBuffer_;
    QHash<QTcpSocket*, QByteArray> peerBuffers_;
    QHash<QString, Peer> peers_;
    QString myName_;
    QString serverHost_ = "127.0.0.1";
    quint16 serverPort_ = 9000;
    QString advertiseHost_;
    quint16 preferredVoicePort_ = 5060;
    QString relayHost_;
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
    bool localSpeaking_ = false;
    bool muted_ = false;
    bool deafened_ = false;
    bool mutedBeforeDeafen_ = false;
    VoiceEngine voice_;
    RelayAudio relayAudio_;
    QTimer speakingExpiry_;
    QTimer reconnectTimer_;
    QTimer relayProbeTimer_;
    QTimer loginTimer_;
};
