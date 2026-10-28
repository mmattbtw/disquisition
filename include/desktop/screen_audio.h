#pragma once

#include <QAudioDevice>
#include <QAudioFormat>
#include <QByteArray>
#include <QList>
#include <QObject>
#include <QString>

#include <map>
#include <memory>

class QAudioSink;
class QIODevice;

// Opus through FFmpeg, from the 48 kHz stereo that share capture delivers.
// Not thread-safe: one thread uses it at a time.
class ScreenAudioEncoder {
public:
    ScreenAudioEncoder();
    ~ScreenAudioEncoder();
    ScreenAudioEncoder(const ScreenAudioEncoder&) = delete;
    ScreenAudioEncoder& operator=(const ScreenAudioEncoder&) = delete;

    // False when FFmpeg has no Opus encoder.
    bool open();
    // Buffers `frames` interleaved samples and returns each 20 ms packet
    // they complete.
    QList<QByteArray> encode(const float* interleaved, int frames);

private:
    struct State;
    std::unique_ptr<State> state_;
};

// Turns one sharer's Opus packets back into PCM in `format`.
class ScreenAudioDecoder {
public:
    explicit ScreenAudioDecoder(const QAudioFormat& format);
    ~ScreenAudioDecoder();
    ScreenAudioDecoder(const ScreenAudioDecoder&) = delete;
    ScreenAudioDecoder& operator=(const ScreenAudioDecoder&) = delete;

    // Empty when the packet cannot be decoded.
    QByteArray decode(const QByteArray& packet);

private:
    struct State;
    std::unique_ptr<State> state_;
};

// Plays the sound of the shares being watched, each through its own sink.
class ScreenAudioPlayer final : public QObject {
    Q_OBJECT
public:
    explicit ScreenAudioPlayer(QObject* parent = nullptr);
    ~ScreenAudioPlayer() override;
    // Applies to shares that start playing afterwards.
    void setOutputDevice(const QString& id);
    void play(const QString& sharer, const QByteArray& packet);
    void remove(const QString& sharer);

private:
    struct Stream {
        std::unique_ptr<QAudioSink> sink;
        QIODevice* device = nullptr; // owned by the sink
        std::unique_ptr<ScreenAudioDecoder> decoder;
    };
    QAudioDevice device_;
    QAudioFormat format_;
    std::map<QString, Stream> streams_;
};
