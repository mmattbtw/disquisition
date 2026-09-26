#pragma once

#include <QByteArray>
#include <QElapsedTimer>
#include <QHash>
#include <QImage>
#include <QList>
#include <QObject>
#include <QSize>
#include <QString>

#include <functional>
#include <utility>

#include "common/protocol.h"

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
// down, JPEG-encoded and split into chunks that each fit one protocol frame.
class ScreenCapture final : public QObject {
    Q_OBJECT
public:
    explicit ScreenCapture(QObject* parent = nullptr);
    bool start(QScreen* screen = nullptr);
    void stop();
    bool running() const;
    void setFrameRate(int fps) { frameIntervalMs_ = 1000 / qBound(1, fps, 30); }
    void setMaxSize(const QSize& size) { maxSize_ = size; }
    void setQuality(int quality) { quality_ = qBound(1, quality, 100); }
    void setBacklogProbe(std::function<qint64()> probe) { backlog_ = std::move(probe); }

signals:
    void frameReady(quint32 frameId, const QList<QByteArray>& chunks);
    void errorOccurred(const QString& message);

private:
    void handleFrame(const QVideoFrame& frame);

    QScreenCapture* capture_ = nullptr;
    QMediaCaptureSession* session_ = nullptr;
    QVideoSink* sink_ = nullptr;
    std::function<qint64()> backlog_;
    QElapsedTimer clock_;
    qint64 nextDueMs_ = 0;
    qint64 frameIntervalMs_ = 100;
    QSize maxSize_ {1280, 720};
    int quality_ = 60;
    quint32 nextFrameId_ = 0;
};

// Rebuilds shared frames from their chunks. Only the newest frame from each
// sender is kept; a frame overtaken before it is complete is dropped.
class ScreenFrameAssembler {
public:
    bool add(const QString& sender, quint32 frameId, int index, int count, const QByteArray& chunk,
             QImage& image);
    void remove(const QString& sender) { partial_.remove(sender); }

private:
    struct Partial {
        quint32 frameId = 0;
        int received = 0;
        QList<QByteArray> chunks;
    };
    QHash<QString, Partial> partial_;
};
