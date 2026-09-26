#include "desktop/screen_capture.h"

#include <QBuffer>
#include <QGuiApplication>
#include <QMediaCaptureSession>
#include <QScreen>
#include <QScreenCapture>
#include <QVideoFrame>
#include <QVideoSink>

ScreenCapture::ScreenCapture(QObject* parent) : QObject(parent) {}

bool ScreenCapture::start(QScreen* screen) {
    stop();
    if (!screen) {
        screen = QGuiApplication::primaryScreen();
    }
    if (!screen) {
        return false;
    }
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
    if (capture_) {
        capture_->stop();
    }
    delete session_;
    delete sink_;
    delete capture_;
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
    nextDueMs_ += frameIntervalMs_;
    if (nextDueMs_ <= now) {
        // After a stall, restart the schedule instead of sending a burst.
        nextDueMs_ = now + frameIntervalMs_;
    }

    QImage image = frame.toImage();
    if (image.isNull()) {
        return;
    }
    if (image.width() > maxSize_.width() || image.height() > maxSize_.height()) {
        image = image.scaled(maxSize_, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    }
    QByteArray jpeg;
    QBuffer buffer(&jpeg);
    buffer.open(QIODevice::WriteOnly);
    if (!image.save(&buffer, "JPEG", quality_)) {
        return;
    }

    QList<QByteArray> chunks;
    for (qsizetype offset = 0; offset < jpeg.size(); offset += kScreenChunkBytes) {
        chunks.append(jpeg.mid(offset, kScreenChunkBytes));
    }
    if (chunks.size() > kMaxScreenChunks) {
        return;
    }
    emit frameReady(nextFrameId_++, chunks);
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
