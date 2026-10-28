#pragma once

#include <QBuffer>
#include <QByteArray>
#include <QHash>
#include <QImage>
#include <QImageReader>
#include <QList>
#include <QObject>
#include <QString>
#include <deque>
#include <map>
#include <memory>
#include <optional>
#include <utility>

#include "common/media_limits.h"

class QTcpSocket;

// Images people drop into the chat travel to every peer as PeerImage chunks.

// Larger images are refused before decoding, so a small file cannot claim
// to expand into gigabytes of pixels.
constexpr int kMaxImageSide = 16384;
constexpr qint64 kMaxImagePixels = 64LL * 1024 * 1024;

// Reads an image held in memory. Sizes and pixels come out as the image is
// displayed, with its EXIF rotation applied.
class ImageBytes {
public:
    explicit ImageBytes(const QByteArray& bytes);
    ImageBytes(const ImageBytes&) = delete;
    ImageBytes& operator=(const ImageBytes&) = delete;

    bool canRead() const { return reader_.canRead(); }
    QByteArray format() const { return reader_.format().toLower(); }
    QSize size() const;
    bool isAnimated() const;
    QImage read() { return reader_.read(); }

private:
    QBuffer buffer_;
    QImageReader reader_;
};

struct PreparedImage {
    QByteArray bytes; // empty when `error` says why the image cannot be sent
    QString error;
};

// Returns PNG, JPEG and GIF files of at most chat::kMaxImageBytes unchanged.
// Other still images, and larger ones, are scaled down and re-encoded until
// they fit. Animations other than GIF send their first frame.
PreparedImage prepareImageFile(const QString& path);
PreparedImage prepareImageBytes(const QByteArray& bytes);
PreparedImage prepareImage(const QImage& image);

// True when `bytes` decode as an image within the limits above.
bool isDisplayableImage(const QByteArray& bytes);

// Splits `bytes` into chunks of chat::kImageChunkSize; the last may be shorter.
QList<QByteArray> splitImage(const QByteArray& bytes);

// Reassembles images from chunks while bounding the memory any sender can
// make us hold: a few transfers per sender, a cap on all buffered bytes, and
// a deadline for each transfer.
class ImageAssembler {
public:
    static constexpr int kMaxTransfersPerSender = 2;
    static constexpr qint64 kMaxBufferedBytes = 24LL * 1024 * 1024;
    static constexpr qint64 kTransferTimeoutMs = 120000;

    // Returns the image when `data` completes its transfer.
    std::optional<QByteArray> add(const QString& sender, quint32 transferId, quint32 index,
                                  quint32 count, const QByteArray& data, qint64 nowMs);
    void remove(const QString& sender);
    void clear();
    qint64 bufferedBytes() const { return bufferedBytes_; }

private:
    using Key = std::pair<QString, quint32>;
    struct Partial {
        QList<QByteArray> chunks;
        quint32 received = 0;
        qint64 bytes = 0;
        qint64 startedMs = 0;
    };

    void drop(std::map<Key, Partial>::iterator transfer);
    void expire(qint64 nowMs);

    std::map<Key, Partial> transfers_;
    qint64 bufferedBytes_ = 0;
};

// Writes queued frames without letting any socket buffer more than
// kHighWaterBytes, so voice and screen frames sent meanwhile do not wait
// behind a whole image.
class FramePacer final : public QObject {
    Q_OBJECT

public:
    static constexpr qint64 kHighWaterBytes = 256 * 1024;
    using Frames = std::shared_ptr<const QList<QByteArray>>;

    explicit FramePacer(QObject* parent = nullptr);

    void enqueue(QTcpSocket* socket, Frames frames);
    // Drops everything not yet written.
    void clear();

private:
    struct Pending {
        Frames frames;
        qsizetype next = 0;
    };

    void pump(QTcpSocket* socket);

    QHash<QTcpSocket*, std::deque<Pending>> queues_;
};
