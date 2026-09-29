#pragma once

#include <QAudioFormat>
#include <QAudioDevice>
#include <QAudioSink>
#include <QAudioSource>
#include <QByteArray>
#include <QHash>
#include <QObject>
#include <QString>
#include <QVector>

class QIODevice;

// Audio carried in framed TCP messages. No SIP or RTP socket is opened here.
class RelayAudio final : public QObject {
    Q_OBJECT
public:
    explicit RelayAudio(QObject* parent = nullptr);
    bool start(const QString& inputId, const QString& outputId);
    void stop();
    void receive(const QString& sender, const QByteArray& pcm);
    void remove(const QString& sender);
    void setMuted(bool muted) { muted_ = muted; }
    void setDeafened(bool deafened) { deafened_ = deafened; }
    bool running() const { return source_ != nullptr; }

signals:
    void frameReady(const QByteArray& pcm);
    void speakingChanged(bool speaking);

private:
    struct Playback {
        QAudioSink* sink = nullptr;
        QIODevice* device = nullptr;
        QVector<float> samples;
        double position = 0;
    };
    QAudioFormat inputFormat_;
    QAudioFormat outputFormat_;
    QAudioDevice outputDevice_;
    QAudioSource* source_ = nullptr;
    QIODevice* capture_ = nullptr;
    QHash<QString, Playback> playback_;
    QByteArray captureBytes_;
    QVector<float> captureSamples_;
    double capturePosition_ = 0;
    QByteArray pending_;
    bool muted_ = false;
    bool deafened_ = false;
    bool speaking_ = false;
};
