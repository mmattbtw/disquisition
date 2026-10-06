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
#include <optional>
#include <utility>

#include "common/protocol.h"
#include "desktop/screen_h264.h"
#include "desktop/share_source.h"

class QMediaCaptureSession;
class QScreen;
class QScreenCapture;
class QVideoFrame;
class QVideoSink;
class QWindowCapture;
class MacScreenCapture;

// Requests macOS Screen Recording access when needed. Other platforms return true.
bool requestScreenRecordingAccess();


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
    bool start(const ShareSource& source);
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
    void handleImage(QImage image);
    void submitFrame(std::function<QImage()> image);

    // One thread, never retired, so the encoder always runs on the thread it
    // was opened on and frames are encoded in order.
    QThreadPool encoder_;
    bool encoding_ = false;
    quint64 generation_ = 0;
    // Set while sharing a screen whose frames leave out the mouse pointer.
    QPointer<QScreen> screen_;
    QScreenCapture* screenCapture_ = nullptr;
    QWindowCapture* windowCapture_ = nullptr;
    QMediaCaptureSession* session_ = nullptr;
    QVideoSink* sink_ = nullptr;
#if defined(Q_OS_MACOS)
    std::unique_ptr<MacScreenCapture> macCapture_;
#endif
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

// Decodes one sender's stream. A frame decoded without the one before it comes
// out corrupted, so after any gap in frame ids the stream waits for the next
// keyframe. Viewers who start watching mid-stream wait for one the same way.
class ScreenStreamDecoder {
public:
    // Returns a null image when there is nothing new to show.
    QImage decode(quint32 frameId, bool keyframe, const QByteArray& data);

private:
    ScreenDecoder decoder_;
    std::optional<quint32> lastFrameId_;
};
