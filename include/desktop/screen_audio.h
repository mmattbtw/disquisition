#pragma once

#include <QAudioDevice>
#include <QAudioFormat>
#include <QByteArray>
#include <QIODevice>
#include <QList>
#include <QMutex>
#include <QObject>
#include <QString>

#include <map>
#include <memory>

class QAudioSink;

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

// What a sink plays from. Packets arrive unevenly, held up behind video, so
// playback waits for a cushion of buffered sound, and plays silence rather
// than running dry, which on Windows replays stale sound. The cushion starts
// at `prefill` and grows by a quarter of it each time it runs dry, up to half
// of `limit`, so it settles at what the connection needs. Beyond `limit` the
// oldest sound is dropped, so the delay cannot grow without bound.
class PlaybackBuffer final : public QIODevice {
public:
    PlaybackBuffer(qsizetype prefill, qsizetype limit, int bytesPerFrame);
    void push(const QByteArray& pcm);
    bool isSequential() const override { return true; }
    // Never empty: a gap is filled with silence.
    qint64 bytesAvailable() const override { return limit_; }

protected:
    // The sink may read on its own thread.
    qint64 readData(char* data, qint64 size) override;
    qint64 writeData(const char*, qint64) override { return -1; }

private:
    mutable QMutex mutex_;
    QByteArray pending_;
    qsizetype cushion_;
    const qsizetype growth_;
    const qsizetype limit_;
    const int bytesPerFrame_;
    bool playing_ = false;
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
    // The sink is declared last so it stops before its buffer goes.
    struct Stream {
        std::unique_ptr<ScreenAudioDecoder> decoder;
        std::unique_ptr<PlaybackBuffer> buffer;
        std::unique_ptr<QAudioSink> sink;
    };
    QAudioDevice device_;
    QAudioFormat format_;
    std::map<QString, Stream> streams_;
};
