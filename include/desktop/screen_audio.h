#pragma once

#include <QAudioDevice>
#include <QAudioFormat>
#include <QByteArray>
#include <QIODevice>
#include <QList>
#include <QMutex>
#include <QObject>
#include <QString>
#include <QThread>

#include <functional>
#include <limits>
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

// What a sink plays from. Packets arrive unevenly, so playback waits for a
// cushion of buffered sound, and plays silence rather than running dry, which
// on Windows replays stale sound. The cushion starts at `prefill`. Each time
// it runs dry it grows by the length of the gap plus a step of a quarter of
// `prefill`, up to `maxCushion`, so one gap is enough to cover the next like
// it. After `settle` bytes of steady playback in which it never came within a
// step of running out, it shrinks by a step, back towards `prefill`. More
// than twice the cushion is cut back to the cushion, oldest sound first, so a
// burst after a stall does not linger as delay.
class PlaybackBuffer final : public QIODevice {
public:
    PlaybackBuffer(qsizetype prefill, qsizetype maxCushion, qsizetype settle, int bytesPerFrame);
    void push(const QByteArray& pcm);
    bool isSequential() const override { return true; }
    // Never empty: a gap is filled with silence.
    qint64 bytesAvailable() const override { return 2 * maxCushion_; }

protected:
    // The sink may read on its own thread.
    qint64 readData(char* data, qint64 size) override;
    qint64 writeData(const char*, qint64) override { return -1; }

private:
    mutable QMutex mutex_;
    QByteArray pending_;
    const qsizetype prefill_;
    qsizetype cushion_;
    const qsizetype growth_;
    const qsizetype maxCushion_;
    const qsizetype settle_;
    const int bytesPerFrame_;
    qsizetype steady_ = 0;
    // The least sound buffered during the current stretch of steady playback.
    static constexpr qsizetype kUnmeasured = std::numeric_limits<qsizetype>::max();
    qsizetype lowest_ = kUnmeasured;
    // Silence played since running dry, until sound arrives again.
    qsizetype starved_ = 0;
    bool starving_ = false;
    bool playing_ = false;
};

// Plays the sound of the shares being watched, each through its own sink.
// A sink pulls its sound on the thread that made it, and the UI thread stalls
// while it decodes and paints video, so playback has a thread of its own. The
// methods are called from the UI thread and hand their work over.
class ScreenAudioPlayer final : public QObject {
    Q_OBJECT
public:
    explicit ScreenAudioPlayer(QObject* parent = nullptr);
    ~ScreenAudioPlayer() override;
    // Shares already playing restart on the new device.
    void setOutputDevice(const QString& id);
    // Linear, from 0 to 1. Kept when the sharer's stream restarts.
    void setVolume(const QString& sharer, qreal volume);
    void play(const QString& sharer, const QByteArray& packet);
    void remove(const QString& sharer);

private:
    void post(std::function<void()> work);
    void start(const QString& sharer);

    // The sink is declared last so it stops before its buffer goes.
    struct Stream {
        std::unique_ptr<ScreenAudioDecoder> decoder;
        std::unique_ptr<PlaybackBuffer> buffer;
        std::unique_ptr<QAudioSink> sink;
    };
    QThread thread_;
    // Lives on thread_. Everything below it is used only there.
    QObject* worker_ = nullptr;
    QAudioDevice device_;
    QAudioFormat format_;
    std::map<QString, Stream> streams_;
    std::map<QString, qreal> volumes_;
};
