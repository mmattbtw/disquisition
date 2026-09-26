#pragma once

#include <QByteArray>
#include <QElapsedTimer>
#include <QHash>
#include <QImage>
#include <QList>
#include <QObject>
#include <QPointer>
#include <QSize>
#include <QString>
#include <QThreadPool>

#include <functional>
#include <memory>
#include <utility>

#include "common/protocol.h"
#include "desktop/screen_codec.h"

class QMediaCaptureSession;
class QScreen;
class QScreenCapture;
class QVideoFrame;
class QVideoSink;


constexpr qsizetype kScreenChunkBytes = chat::kMaxFrameSize - 256;
constexpr int kMaxScreenChunks = 64;
// Frames are skipped while more than this is still queued on the socket, so
// chat and voice never wait behind more than about 100 ms of video at 5 Mbps.
constexpr qint64 kMaxScreenBacklogBytes = 64 * 1024;

// Screen sharing carried in framed TCP messages. Each captured frame is scaled
// down, encoded as H.264 and split into chunks that each fit one protocol
// frame. Encoding runs on a worker thread; frames that arrive while it is busy
// are dropped, so the frame rate falls instead of the UI stalling.
class ScreenCapture final : public QObject {
    Q_OBJECT
public:
    explicit ScreenCapture(QObject* parent = nullptr);
    ~ScreenCapture() override;
    bool start(QScreen* screen = nullptr);
    void stop();
    bool running() const;
    // The encoder in use, such as "h264_nvenc".
    QString encoderName() const { return encoderName_; }
    void setFrameRate(int fps) { frameIntervalMs_ = 1000 / qBound(1, fps, 60); }
    void setMaxSize(const QSize& size) { maxSize_ = size; }
    void setBacklogProbe(std::function<qint64()> probe) { backlog_ = std::move(probe); }

signals:
    // Frames depend on the ones before them; keyframes do not.
    void frameReady(quint32 frameId, const QList<QByteArray>& chunks, bool keyframe);
    void errorOccurred(const QString& message);

private:
    void handleFrame(const QVideoFrame& frame);

    // One thread, never retired, so the encoder always runs on the thread it
    // was opened on and frames are encoded in order.
    QThreadPool encoder_;
    bool encoding_ = false;
    quint64 generation_ = 0;
    QPointer<QScreen> screen_;
    QScreenCapture* capture_ = nullptr;
    QMediaCaptureSession* session_ = nullptr;
    QVideoSink* sink_ = nullptr;
    std::function<qint64()> backlog_;
    QElapsedTimer clock_;
    qint64 nextDueMs_ = 0;
    // Used only by the encoding thread; replaced on each start().
    std::shared_ptr<H264Encoder> h264_;
    QString encoderName_;
    qint64 frameIntervalMs_ = 1000 / 30;
    QSize maxSize_ {1920, 1080};
    quint32 nextFrameId_ = 0;
};

// Rebuilds shared frames from their chunks. Only the newest frame from each
// sender is kept; a frame overtaken before it is complete is dropped.
class ScreenFrameAssembler {
public:
    // Returns true and sets `data` to the encoded frame when `chunk`
    // completes it.
    bool add(const QString& sender, quint32 frameId, int index, int count, const QByteArray& chunk,
             QByteArray& data);
    void remove(const QString& sender) { partial_.remove(sender); }

private:
    struct Partial {
        quint32 frameId = 0;
        int received = 0;
        QList<QByteArray> chunks;
    };
    QHash<QString, Partial> partial_;
};
