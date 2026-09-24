#pragma once

#include <QObject>
#include <QProcess>
#include <QTcpSocket>
#include <QTimer>
#include <QString>
#include <QSet>

class VoiceEngine final : public QObject {
    Q_OBJECT

public:
    explicit VoiceEngine(QObject* parent = nullptr);
    bool start(const QString& name, quint16 sipPort, const QString& inputDevice,
               const QString& outputDevice, QString& error);
    void callPeer(const QString& name, const QString& host, quint16 sipPort);
    void setMuted(bool muted);
    void setDeafened(bool deafened);
    void stop();
    bool running() const;

signals:
    void ready();
    void stopped();
    void statusChanged(const QString& status);
    void logMessage(const QString& line);
    void localSpeakingChanged(bool speaking);
    void peerVoiceChanged(const QString& name, bool connected, bool speaking);

private:
    void command(const QString& text);
    bool writeProfile(const QString& name, quint16 sipPort, const QString& inputDevice,
                      const QString& outputDevice, QString& error);
    void connectControl();
    void readControl();
    void handleControlEvent(const QByteArray& json);
    static QString peerName(const QString& uri);

    QProcess process_;
    QTcpSocket control_;
    QTimer controlRetry_;
    QByteArray controlBuffer_;
    QString profilePath_;
    QString lastOutput_;
    QString outputBuffer_;
    QSet<QString> dialed_;
    QSet<QString> pendingCalls_;
    QSet<QString> callIds_;
    quint16 controlPort_ = 0;
    bool muted_ = false;
    bool deafened_ = false;
    bool mutedBeforeDeafen_ = false;
    bool initialized_ = false;
};
