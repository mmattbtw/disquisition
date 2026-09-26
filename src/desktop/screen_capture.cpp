#include "desktop/screen_capture.h"

#include <QBuffer>
#include <QCursor>
#include <QGuiApplication>
#include <QMediaCaptureSession>
#include <QPainter>
#include <QPolygonF>
#include <QScreen>
#include <QScreenCapture>
#include <QThread>
#include <QVideoFrame>
#include <QVideoSink>

namespace {

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
    // on are converted. Windows delivers RGBA8888.
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

// Runs on the worker thread. Returns no chunks when the frame cannot be sent.
QList<QByteArray> encodeFrame(const QVideoFrame& frame, const QRect& geometry,
                              const QPoint& cursor, const QSize& maxSize, int quality) {
    QImage image = frame.toImage();
    if (image.isNull()) {
        return {};
    }
    drawCursor(image, geometry, cursor);
    if (image.width() > maxSize.width() || image.height() > maxSize.height()) {
        image = image.scaled(maxSize, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    }
    QByteArray jpeg;
    QBuffer buffer(&jpeg);
    buffer.open(QIODevice::WriteOnly);
    if (!image.save(&buffer, "JPEG", quality)) {
        return {};
    }

    QList<QByteArray> chunks;
    for (qsizetype offset = 0; offset < jpeg.size(); offset += kScreenChunkBytes) {
        chunks.append(jpeg.mid(offset, kScreenChunkBytes));
    }
    if (chunks.size() > kMaxScreenChunks) {
        return {};
    }
    return chunks;
}

} // namespace

ScreenCapture::ScreenCapture(QObject* parent) : QObject(parent) {
    // A 1080p frame takes 40-80 ms to encode, so one thread manages about
    // 15 fps; three keep up with 30 fps while leaving cores for the rest.
    encoders_.setMaxThreadCount(qBound(1, QThread::idealThreadCount() - 1, 3));
}

ScreenCapture::~ScreenCapture() {
    stop();
    // Jobs post their result back to this object, so none may still run.
    encoders_.waitForDone();
}

bool ScreenCapture::start(QScreen* screen) {
    stop();
    if (!screen) {
        screen = QGuiApplication::primaryScreen();
    }
    if (!screen) {
        return false;
    }
    screen_ = screen;
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
    return true;
}

void ScreenCapture::stop() {
    ++generation_;
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
    return capture_ && capture_->isActive();
}

void ScreenCapture::handleFrame(const QVideoFrame& frame) {
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
    // Every worker is still on an earlier frame; this one is dropped.
    if (encoding_ >= encoders_.maxThreadCount()) {
        return;
    }
    nextDueMs_ += frameIntervalMs_;
    if (nextDueMs_ <= now) {
        // After a stall, restart the schedule instead of sending a burst.
        nextDueMs_ = now + frameIntervalMs_;
    }

    ++encoding_;
    QRect geometry;
    QPoint cursor;
    if (screen_) {
        geometry = screen_->geometry();
        cursor = QCursor::pos(screen_);
    }
    const quint32 frameId = nextFrameId_++;
    const quint64 generation = generation_;
    encoders_.start([this, frame, geometry, cursor, frameId, generation, maxSize = maxSize_,
                     quality = quality_] {
        QList<QByteArray> chunks = encodeFrame(frame, geometry, cursor, maxSize, quality);
        QMetaObject::invokeMethod(this, [this, chunks = std::move(chunks), frameId, generation] {
            --encoding_;
            if (generation == generation_ && !chunks.isEmpty() && frameId > lastEmittedId_) {
                lastEmittedId_ = frameId;
                emit frameReady(frameId, chunks);
            }
        });
    });
}

bool ScreenFrameAssembler::add(const QString& sender, quint32 frameId, int index, int count,
                               const QByteArray& chunk, QImage& image) {
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

    QByteArray jpeg;
    for (const QByteArray& piece : std::as_const(partial.chunks)) {
        jpeg += piece;
    }
    partial_.remove(sender);
    image = QImage::fromData(jpeg, "JPEG");
    return !image.isNull();
}
