#include "desktop/screen_capture.h"

#include <QCursor>
#include <QGuiApplication>
#include <QMediaCaptureSession>
#include <QPainter>
#include <QPolygonF>
#include <QScreen>
#include <QScreenCapture>
#include <QVideoFrame>
#include <QVideoFrameFormat>
#include <QVideoSink>

#if defined(Q_OS_MACOS)
#include "desktop/mac_screen_capture.h"
#include <CoreGraphics/CGWindow.h>
#endif

bool requestScreenRecordingAccess() {
#if defined(Q_OS_MACOS)
    return CGPreflightScreenCaptureAccess() || CGRequestScreenCaptureAccess();
#else
    return true;
#endif
}

namespace {

struct EncodedFrame {
    QList<QByteArray> chunks;
    bool keyframe = false;
    bool failed = false;
    QString encoder;
};

// Copies the frame's pixels out directly when Qt has a matching image format,
// which is several times faster than QVideoFrame::toImage().
QImage frameImage(const QVideoFrame& frame) {
    QVideoFrame mapped(frame);
    if (mapped.map(QVideoFrame::ReadOnly)) {
        const QImage::Format format =
            QVideoFrameFormat::imageFormatFromPixelFormat(mapped.pixelFormat());
        QImage image;
        if (format != QImage::Format_Invalid) {
            image = QImage(mapped.bits(0), mapped.width(), mapped.height(), mapped.bytesPerLine(0),
                           format)
                        .copy();
        }
        mapped.unmap();
        if (!image.isNull()) {
            return image;
        }
    }
    return frame.toImage();
}

// Captured frames leave out the mouse pointer, so draw an arrow where it is.
void drawCursor(QImage& image, const QRect& geometry, const QPoint& position) {
    if (geometry.isEmpty() || !geometry.contains(position)) {
        return;
    }
    // The frame is in device pixels and the cursor position in logical ones.
    const double scale = static_cast<double>(image.width()) / geometry.width();
    const QPointF tip = QPointF(position - geometry.topLeft()) * scale;
    QPolygonF arrow {{0, 0}, {0, 17}, {4, 13}, {7, 20}, {10, 19}, {7, 12}, {12, 12}};
    for (QPointF& point : arrow) {
        point = tip + point * scale;
    }
    // Converting a full frame is slow, so only formats QPainter cannot draw
    // on are converted. Windows delivers BGRA, which is ARGB32 here.
    switch (image.format()) {
        case QImage::Format_RGB32:
        case QImage::Format_ARGB32:
        case QImage::Format_ARGB32_Premultiplied:
        case QImage::Format_RGBX8888:
        case QImage::Format_RGBA8888:
        case QImage::Format_RGBA8888_Premultiplied:
            break;
        default:
            image = image.convertToFormat(QImage::Format_RGB32);
    }
    QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(QPen(Qt::black, scale));
    painter.setBrush(Qt::white);
    painter.drawPolygon(arrow);
}

QList<QByteArray> splitChunks(const QByteArray& data) {
    QList<QByteArray> chunks;
    for (qsizetype offset = 0; offset < data.size(); offset += kScreenChunkBytes) {
        chunks.append(data.mid(offset, kScreenChunkBytes));
    }
    if (chunks.size() > kMaxScreenChunks) {
        return {};
    }
    return chunks;
}

// Runs on the encoding thread. Returns no chunks when there is nothing to send.
EncodedFrame encodeFrame(QImage image, const QRect& geometry, const QPoint& cursor,
                         H264Encoder& h264, const QSize& maxSize, int fps, qint64 ptsMs) {
    EncodedFrame encoded;
    if (image.isNull()) {
        return encoded;
    }
    drawCursor(image, geometry, cursor);

    QByteArray data;
    if (!h264.encode(image, maxSize, fps, ptsMs, data, encoded.keyframe)) {
        encoded.failed = true;
        return encoded;
    }
    encoded.encoder = h264.name();
    encoded.chunks = splitChunks(data);
    return encoded;
}

} // namespace

ScreenCapture::ScreenCapture(QObject* parent) : QObject(parent) {
    encoder_.setMaxThreadCount(1);
    encoder_.setExpiryTimeout(-1);
}

ScreenCapture::~ScreenCapture() {
    stop();
    // Jobs post their result back to this object, so none may still run.
    encoder_.waitForDone();
}

bool ScreenCapture::start(QScreen* screen) {
    stop();
    if (!screen) {
        screen = QGuiApplication::primaryScreen();
    }
    if (!screen) {
        return false;
    }
    // A fresh encoder starts the stream with a keyframe.
    h264_ = std::make_shared<H264Encoder>();
    encoderName_.clear();

    screen_ = screen;
#if defined(Q_OS_MACOS)
    const quint64 generation = generation_;
    QPointer<ScreenCapture> self(this);
    macCapture_ = std::make_unique<MacScreenCapture>(
        [self, generation](QImage image) {
            QMetaObject::invokeMethod(QGuiApplication::instance(),
                                      [self, generation, image = std::move(image)]() mutable {
                if (self && self->generation_ == generation) self->handleImage(std::move(image));
            });
        },
        [self, generation](QString error) {
            QMetaObject::invokeMethod(QGuiApplication::instance(), [self, generation, error] {
                if (self && self->generation_ == generation) self->errorOccurred(error);
            });
        });
    clock_.start();
    nextDueMs_ = 0;
    macCapture_->start();
#else
    capture_ = new QScreenCapture(this);
    session_ = new QMediaCaptureSession(this);
    sink_ = new QVideoSink(this);
    capture_->setScreen(screen);
    session_->setScreenCapture(capture_);
    session_->setVideoSink(sink_);
    connect(capture_, &QScreenCapture::errorOccurred, this,
            [this](QScreenCapture::Error, const QString& message) { emit errorOccurred(message); });
    connect(sink_, &QVideoSink::videoFrameChanged, this, &ScreenCapture::handleFrame);
    clock_.start();
    nextDueMs_ = 0;
    capture_->start();
#endif
    return true;
}

void ScreenCapture::stop() {
    ++generation_;
#if defined(Q_OS_MACOS)
    macCapture_.reset();
#endif
    if (capture_) {
        capture_->stop();

        capture_->disconnect(this);
        sink_->disconnect(this);
        session_->deleteLater();
        sink_->deleteLater();
        capture_->deleteLater();
    }
    session_ = nullptr;
    sink_ = nullptr;
    capture_ = nullptr;
}

bool ScreenCapture::running() const {
#if defined(Q_OS_MACOS)
    return macCapture_ && macCapture_->running();
#else
    return capture_ && capture_->isActive();
#endif
}

void ScreenCapture::handleFrame(const QVideoFrame& frame) {
    submitFrame([frame] { return frameImage(frame); });
}

void ScreenCapture::handleImage(QImage image) {
    if (image.isNull()) return;
    submitFrame([image = std::move(image)] { return image; });
}

void ScreenCapture::submitFrame(std::function<QImage()> image) {
    // The screen delivers frames at its own refresh rate. Keep one per
    // interval, scheduled from the previous deadline so the average rate holds
    // even though frames never land exactly on it.
    const qint64 now = clock_.elapsed();
    if (now < nextDueMs_) {
        return;
    }
    // A congested socket skips this frame; the next one tries again.
    if (backlog_ && backlog_() > kMaxScreenBacklogBytes) {
        return;
    }
    // The encoder is still on an earlier frame; this one is dropped.
    if (encoding_) {
        return;
    }
    nextDueMs_ += frameIntervalMs_;
    if (nextDueMs_ <= now) {
        // After a stall, restart the schedule instead of sending a burst.
        nextDueMs_ = now + frameIntervalMs_;
    }

    encoding_ = true;
    QRect geometry;
    QPoint cursor;
    if (screen_) {
        geometry = screen_->geometry();
        cursor = QCursor::pos(screen_);
    }
    const quint32 frameId = nextFrameId_++;
    const quint64 generation = generation_;
    const int fps = static_cast<int>(1000 / frameIntervalMs_);
    encoder_.start([this, image = std::move(image), geometry, cursor, frameId, generation, h264 = h264_,
                    maxSize = maxSize_, fps, now] {
        EncodedFrame encoded = encodeFrame(image(), geometry, cursor, *h264, maxSize, fps, now);
        QMetaObject::invokeMethod(this, [this, encoded = std::move(encoded), frameId, generation] {
            encoding_ = false;
            if (generation != generation_) {
                return;
            }
            if (encoded.failed) {
                emit errorOccurred(QStringLiteral("no H.264 encoder could be opened"));
                return;
            }
            if (!encoded.encoder.isEmpty()) {
                encoderName_ = encoded.encoder;
            }
            if (!encoded.chunks.isEmpty()) {
                emit frameReady(frameId, encoded.chunks, encoded.keyframe);
            }
        });
    });
}

bool ScreenFrameAssembler::add(const QString& sender, quint32 frameId, int index, int count,
                               const QByteArray& chunk, QByteArray& data) {
    if (count < 1 || count > kMaxScreenChunks || index < 0 || index >= count || chunk.isEmpty() ||
        chunk.size() > kScreenChunkBytes) {
        return false;
    }
    Partial& partial = partial_[sender];
    if (partial.chunks.isEmpty() || partial.frameId != frameId || partial.chunks.size() != count) {
        partial = Partial {frameId, 0, QList<QByteArray>(count)};
    }
    if (!partial.chunks[index].isEmpty()) {
        return false;
    }
    partial.chunks[index] = chunk;
    if (++partial.received < count) {
        return false;
    }

    data.clear();
    for (const QByteArray& piece : std::as_const(partial.chunks)) {
        data += piece;
    }
    partial_.remove(sender);
    return true;
}
