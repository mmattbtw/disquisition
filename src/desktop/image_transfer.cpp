#include "desktop/image_transfer.h"

#include <QBuffer>
#include <QFile>
#include <QFileInfo>
#include <QImageReader>
#include <QTcpSocket>
#include <algorithm>
#include <vector>

#include "common/media_limits.h"

namespace {

constexpr qint64 kMaxImageBytes = static_cast<qint64>(chat::kMaxImageBytes);
constexpr qsizetype kChunkSize = static_cast<qsizetype>(chat::kImageChunkSize);
// Re-encoded images are scaled to at most this many pixels on their longer side.
constexpr int kMaxSentSide = 2560;
// Files this large are refused before reading them into memory.
constexpr qint64 kMaxSourceFileBytes = 64LL * 1024 * 1024;

struct ImageInfo {
    QByteArray format;
    bool animated = false;
};

bool withinLimits(const QSize& size) {
    return size.isValid() && size.width() <= kMaxImageSide && size.height() <= kMaxImageSide &&
           static_cast<qint64>(size.width()) * size.height() <= kMaxImagePixels;
}

// Reads only the header, so oversized images are refused before decoding.
std::optional<ImageInfo> inspect(const QByteArray& bytes) {
    const ImageBytes image(bytes);
    if (!image.canRead() || !withinLimits(image.size())) {
        return std::nullopt;
    }
    return ImageInfo{image.format(), image.isAnimated()};
}

QByteArray encode(const QImage& image, const char* format, int quality) {
    QByteArray bytes;
    QBuffer buffer(&bytes);
    buffer.open(QIODevice::WriteOnly);
    return image.save(&buffer, format, quality) ? bytes : QByteArray();
}

// Lossless PNG keeps screenshots sharp. When it is too large, opaque images
// fall back to JPEG; after that the image shrinks until it fits.
PreparedImage encodeToFit(QImage image) {
    if (image.isNull()) {
        return {{}, "the image could not be read"};
    }
    if (std::max(image.width(), image.height()) > kMaxSentSide) {
        image = image.scaled(kMaxSentSide, kMaxSentSide, Qt::KeepAspectRatio,
                             Qt::SmoothTransformation);
    }
    for (int attempt = 0; attempt < 8 && std::min(image.width(), image.height()) >= 16; ++attempt) {
        QByteArray bytes = encode(image, "PNG", -1);
        if (bytes.size() > kMaxImageBytes && !image.hasAlphaChannel()) {
            bytes = encode(image, "JPEG", 85);
        }
        if (bytes.isEmpty()) {
            return {{}, "the image could not be encoded"};
        }
        if (bytes.size() <= kMaxImageBytes) {
            return {bytes, {}};
        }
        image = image.scaled(image.size() * 3 / 4, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    }
    return {{}, "the image is too large to send"};
}

} // namespace

ImageBytes::ImageBytes(const QByteArray& bytes) {
    buffer_.setData(bytes);
    buffer_.open(QIODevice::ReadOnly);
    reader_.setDevice(&buffer_);
    reader_.setAutoTransform(true);
}

QSize ImageBytes::size() const {
    const QSize size = reader_.size();
    return reader_.transformation() & QImageIOHandler::TransformationRotate90 ? size.transposed()
                                                                              : size;
}

bool ImageBytes::isAnimated() const {
    return reader_.supportsAnimation() && reader_.imageCount() != 1;
}

PreparedImage prepareImageFile(const QString& path) {
    QFile file(path);
    const QString name = QFileInfo(path).fileName();
    if (file.size() > kMaxSourceFileBytes) {
        return {{}, name + " is too large to send"};
    }
    if (!file.open(QIODevice::ReadOnly)) {
        return {{}, name + " could not be opened: " + file.errorString()};
    }
    PreparedImage prepared = prepareImageBytes(file.readAll());
    if (!prepared.error.isEmpty()) {
        prepared.error = name + ": " + prepared.error;
    }
    return prepared;
}

PreparedImage prepareImageBytes(const QByteArray& bytes) {
    const std::optional<ImageInfo> info = inspect(bytes);
    if (!info) {
        return {{}, "not a supported image, or its dimensions are too large"};
    }
    static const QList<QByteArray> sentAsIs{"png", "jpeg", "jpg", "gif"};
    if (bytes.size() <= kMaxImageBytes && sentAsIs.contains(info->format)) {
        return {bytes, {}};
    }
    if (info->animated && info->format == "gif") {
        return {{}, "animated GIFs must be 4 MB or smaller"};
    }
    return encodeToFit(ImageBytes(bytes).read());
}

PreparedImage prepareImage(const QImage& image) {
    if (!withinLimits(image.size())) {
        return {{}, "the image is empty or its dimensions are too large"};
    }
    return encodeToFit(image);
}

bool isDisplayableImage(const QByteArray& bytes) {
    return inspect(bytes).has_value();
}

QList<QByteArray> splitImage(const QByteArray& bytes) {
    QList<QByteArray> chunks;
    for (qsizetype offset = 0; offset < bytes.size(); offset += kChunkSize) {
        chunks.append(bytes.mid(offset, kChunkSize));
    }
    return chunks;
}

std::optional<QByteArray> ImageAssembler::add(const QString& sender, quint32 transferId,
                                              quint32 index, quint32 count, const QByteArray& data,
                                              qint64 nowMs) {
    expire(nowMs);
    // Every chunk but the last is full, so `count` bounds the image size.
    if (count == 0 || count > chat::kMaxImageChunks || index >= count || data.isEmpty() ||
        data.size() > kChunkSize || (index + 1 < count && data.size() != kChunkSize)) {
        return std::nullopt;
    }

    const Key key{sender, transferId};
    auto transfer = transfers_.find(key);
    if (transfer != transfers_.end() &&
        transfer->second.chunks.size() != static_cast<qsizetype>(count)) {
        drop(transfer);
        transfer = transfers_.end();
    }
    if (transfer == transfers_.end()) {
        // Keep only this sender's newest transfers, leaving room for this one.
        std::vector<std::map<Key, Partial>::iterator> mine;
        for (auto it = transfers_.begin(); it != transfers_.end(); ++it) {
            if (it->first.first == sender) {
                mine.push_back(it);
            }
        }
        std::sort(mine.begin(), mine.end(), [](const auto& left, const auto& right) {
            return left->second.startedMs < right->second.startedMs;
        });
        const std::size_t keep = kMaxTransfersPerSender - 1;
        for (std::size_t i = 0; i + keep < mine.size(); ++i) {
            drop(mine[i]);
        }
        Partial partial;
        partial.chunks.resize(static_cast<qsizetype>(count));
        partial.startedMs = nowMs;
        transfer = transfers_.emplace(key, std::move(partial)).first;
    }

    Partial& partial = transfer->second;
    const auto slot = static_cast<qsizetype>(index);
    if (!partial.chunks[slot].isEmpty()) {
        return std::nullopt;
    }
    if (bufferedBytes_ + data.size() > kMaxBufferedBytes) {
        drop(transfer);
        return std::nullopt;
    }
    partial.chunks[slot] = data;
    partial.bytes += data.size();
    bufferedBytes_ += data.size();
    if (++partial.received < count) {
        return std::nullopt;
    }

    QByteArray image;
    image.reserve(partial.bytes);
    for (const QByteArray& chunk : std::as_const(partial.chunks)) {
        image += chunk;
    }
    drop(transfer);
    return image;
}

void ImageAssembler::remove(const QString& sender) {
    for (auto it = transfers_.begin(); it != transfers_.end();) {
        auto current = it++;
        if (current->first.first == sender) {
            drop(current);
        }
    }
}

void ImageAssembler::clear() {
    transfers_.clear();
    bufferedBytes_ = 0;
}

void ImageAssembler::drop(std::map<Key, Partial>::iterator transfer) {
    bufferedBytes_ -= transfer->second.bytes;
    transfers_.erase(transfer);
}

void ImageAssembler::expire(qint64 nowMs) {
    for (auto it = transfers_.begin(); it != transfers_.end();) {
        auto current = it++;
        if (nowMs - current->second.startedMs > kTransferTimeoutMs) {
            drop(current);
        }
    }
}

FramePacer::FramePacer(QObject* parent) : QObject(parent) {
}

void FramePacer::enqueue(QTcpSocket* socket, Frames frames) {
    if (socket == nullptr || !frames || frames->isEmpty()) {
        return;
    }
    if (!queues_.contains(socket)) {
        connect(socket, &QTcpSocket::bytesWritten, this, [this, socket] { pump(socket); });
        connect(socket, &QTcpSocket::disconnected, this, [this, socket] {
            if (const auto queue = queues_.find(socket); queue != queues_.end()) {
                queue->clear();
            }
        });
        connect(socket, &QObject::destroyed, this, [this, socket] { queues_.remove(socket); });
    }
    queues_[socket].push_back({std::move(frames), 0});
    pump(socket);
}

void FramePacer::clear() {
    for (auto& queue : queues_) {
        queue.clear();
    }
}

void FramePacer::pump(QTcpSocket* socket) {
    for (;;) {
        // Look the queue up again each time: a write can disconnect the socket.
        const auto queue = queues_.find(socket);
        if (queue == queues_.end() || queue->empty()) {
            return;
        }
        if (socket->state() != QAbstractSocket::ConnectedState) {
            queue->clear();
            return;
        }
        if (socket->bytesToWrite() >= kHighWaterBytes) {
            return;
        }
        Pending& pending = queue->front();
        const QByteArray frame = pending.frames->at(pending.next++);
        if (pending.next == pending.frames->size()) {
            queue->pop_front();
        }
        socket->write(frame);
    }
}
