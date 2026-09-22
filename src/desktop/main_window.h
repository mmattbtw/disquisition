#pragma once

#include <QHash>
#include <QMainWindow>
#include <QTcpServer>
#include <QTcpSocket>

#include "common/protocol.h"
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

    QLineEdit* host_ = nullptr;
    QSpinBox* serverPort_ = nullptr;
    QLineEdit* name_ = nullptr;
    QLineEdit* advertise_ = nullptr;
    QSpinBox* voicePort_ = nullptr;
    QComboBox* inputDevice_ = nullptr;
    QComboBox* outputDevice_ = nullptr;
    QPushButton* connectButton_ = nullptr;
    QPushButton* muteButton_ = nullptr;
    QPushButton* deafenButton_ = nullptr;
    QTextBrowser* transcript_ = nullptr;
    QListWidget* members_ = nullptr;
    QLineEdit* composer_ = nullptr;
    QPushButton* sendButton_ = nullptr;
    QLabel* connectionLabel_ = nullptr;
    QLabel* voiceLabel_ = nullptr;
    QMediaDevices* mediaDevices_ = nullptr;

    QTcpSocket server_;
    QTcpServer peerServer_;
    QByteArray serverBuffer_;
    QHash<QTcpSocket*, QByteArray> peerBuffers_;
    QHash<QString, Peer> peers_;
    QString myName_;
    QString messageColor_ = "mint";
    bool localSpeaking_ = false;
    bool muted_ = false;
    bool deafened_ = false;
    bool mutedBeforeDeafen_ = false;
    VoiceEngine voice_;
};
