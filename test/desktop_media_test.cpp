#include <QBuffer>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QImage>
#include <QImageReader>
#include <QRandomGenerator>
#include <QTcpServer>
#include <QTcpSocket>
#include <QUrlQuery>
#include <cstdio>
#include <memory>

#include "check.h"
#include "common/media_limits.h"
#include "desktop/gif_search.h"
#include "desktop/image_transfer.h"
#include "desktop/remote_media.h"

namespace {

constexpr qsizetype kChunk = static_cast<qsizetype>(chat::kImageChunkSize);

// A 1x1 transparent GIF.
const QByteArray kTinyGif = QByteArray::fromHex(
    "47494638396101000100800000000000ffffff21f90401000000002c00000000010001000002024401003b");

QByteArray encodeImage(const QImage& image, const char* format) {
    QByteArray bytes;
    QBuffer buffer(&bytes);
    buffer.open(QIODevice::WriteOnly);
    CHECK(image.save(&buffer, format));
    return bytes;
}

QByteArray formatOf(const QByteArray& bytes) {
    QBuffer buffer;
    buffer.setData(bytes);
    buffer.open(QIODevice::ReadOnly);
    return QImageReader::imageFormat(&buffer);
}

// Random pixels barely compress, which makes the PNG too large to send.
QImage noise(int width, int height, QImage::Format format) {
    QImage image(width, height, format);
    for (int y = 0; y < height; ++y) {
        auto* line = reinterpret_cast<quint32*>(image.scanLine(y));
        for (int x = 0; x < width; ++x) {
            line[x] = QRandomGenerator::global()->generate() |
                      (format == QImage::Format_RGB32 ? 0xFF000000u : 0u);
        }
    }
    return image;
}

void testMediaLinks() {
    CHECK(isTrustedMediaUrl(QUrl("https://media.giphy.com/media/abc/giphy.gif")));
    CHECK(isTrustedMediaUrl(QUrl("https://media3.giphy.com/media/abc/giphy.gif")));
    CHECK(isTrustedMediaUrl(QUrl("https://i.giphy.com/abc.gif")));
    CHECK(isTrustedMediaUrl(QUrl("https://media.tenor.com/abc/cat.gif")));
    CHECK(isTrustedMediaUrl(QUrl("https://static.klipy.com/ii/abc/um0L4dFH.gif")));
    CHECK(!isTrustedMediaUrl(QUrl("http://media.giphy.com/media/abc/giphy.gif")));
    CHECK(!isTrustedMediaUrl(QUrl("https://media.giphy.com.example.com/a.gif")));
    CHECK(!isTrustedMediaUrl(QUrl("https://evilgiphy.com/a.gif")));
    CHECK(!isTrustedMediaUrl(QUrl("https://media10.giphy.com/a.gif")));
    CHECK(!isTrustedMediaUrl(QUrl("https://user@media.giphy.com/a.gif")));
    CHECK(!isTrustedMediaUrl(QUrl("https://media.giphy.com:8443/a.gif")));
    CHECK(!isTrustedMediaUrl(QUrl("https://tenor.com/view/cat-gif-123")));
    CHECK(!isTrustedMediaUrl(QUrl("https://example.com/cat.gif")));

    QList<MediaLink> links = findMediaLinks("look https://media.tenor.com/x/cat.gif!");
    CHECK(links.size() == 1);
    CHECK(links[0].text == "https://media.tenor.com/x/cat.gif");
    CHECK(links[0].url == QUrl("https://media.tenor.com/x/cat.gif"));

    // GIPHY page links resolve to their GIF.
    links = findMediaLinks("https://giphy.com/gifs/cat-dance-3o7TKSjRrfIPjeiVyM");
    CHECK(links.size() == 1);
    CHECK(links[0].url == QUrl("https://media.giphy.com/media/3o7TKSjRrfIPjeiVyM/giphy.gif"));
    CHECK(findMediaLinks("https://giphy.com/embed/3o7TKSjRrfIPjeiVyM").size() == 1);
    CHECK(findMediaLinks("https://giphy.com/gifs/x/y/z").isEmpty());
    CHECK(findMediaLinks("https://giphy.com/gifs/bad!id").isEmpty());

    // Untrusted links are left as text; duplicates and extras are dropped.
    CHECK(findMediaLinks("https://example.com/a.gif http://i.giphy.com/a.gif").isEmpty());
    links = findMediaLinks("https://i.giphy.com/a.gif https://i.giphy.com/a.gif "
                           "https://i.giphy.com/b.gif https://i.giphy.com/c.gif "
                           "https://i.giphy.com/d.gif");
    CHECK(links.size() == kMaxMediaLinksPerMessage);
    CHECK(links[1].url == QUrl("https://i.giphy.com/b.gif"));
}

void testRequestUrls() {
    QUrl url = giphyRequestUrl("key&x", "", 2);
    CHECK(url.host() == "api.giphy.com");
    CHECK(url.path() == "/v1/gifs/trending");
    QUrlQuery query(url);
    CHECK(query.queryItemValue("api_key", QUrl::FullyDecoded) == "key&x");
    CHECK(query.queryItemValue("offset") == QString::number(2 * kGifPageSize));
    CHECK(!query.hasQueryItem("q"));

    url = giphyRequestUrl("key", "  happy cat ", 0);
    CHECK(url.path() == "/v1/gifs/search");
    CHECK(QUrlQuery(url).queryItemValue("q", QUrl::FullyDecoded) == "happy cat");

    url = klipyRequestUrl("a/b", "customer", "dog", 0);
    CHECK(url.host() == "api.klipy.com");
    CHECK(url.path(QUrl::FullyEncoded) == "/api/v1/a%2Fb/gifs/search");
    query = QUrlQuery(url);
    CHECK(query.queryItemValue("page") == "1");
    CHECK(query.queryItemValue("customer_id") == "customer");
    CHECK(query.queryItemValue("q") == "dog");
    CHECK(klipyRequestUrl("key", "c", " ", 3).path() == "/api/v1/key/gifs/trending");
}

void testGiphyResponse() {
    const QByteArray json = R"({
        "data": [
            {"title": "Cat", "images": {
                "fixed_width_downsampled": {"url": "https://media2.giphy.com/media/a/200w_d.gif?cid=x", "width": "200", "height": "150"},
                "downsized": {"url": "https://media2.giphy.com/media/a/giphy-downsized.gif?cid=x&rid=y"}}},
            {"title": "Fallbacks", "images": {
                "fixed_width": {"url": "https://media.giphy.com/media/b/200w.gif", "width": "200", "height": "100"},
                "original": {"url": "https://media.giphy.com/media/b/giphy.gif"}}},
            {"title": "Elsewhere", "images": {
                "fixed_width": {"url": "https://example.com/c.gif"},
                "original": {"url": "https://example.com/c.gif"}}},
            {"title": "Empty", "images": {}}
        ],
        "pagination": {"total_count": 100, "count": 4, "offset": 0}
    })";
    const std::optional<GifPage> page = parseGiphyResponse(json);
    CHECK(page.has_value());
    CHECK(page->results.size() == 2);
    CHECK(page->results[0].title == "Cat");
    CHECK(page->results[0].previewUrl == QUrl("https://media2.giphy.com/media/a/200w_d.gif"));
    CHECK(page->results[0].shareUrl ==
          QUrl("https://media2.giphy.com/media/a/giphy-downsized.gif"));
    CHECK(page->results[0].previewSize == QSize(200, 150));
    CHECK(page->results[1].shareUrl == QUrl("https://media.giphy.com/media/b/giphy.gif"));
    CHECK(page->hasMore);

    CHECK(!parseGiphyResponse(
               R"({"data": [], "pagination": {"total_count": 4, "count": 0, "offset": 4}})")
               ->hasMore);
    CHECK(!parseGiphyResponse("not json"));
    CHECK(!parseGiphyResponse(R"({"meta": {"status": 401}})"));
    CHECK(gifErrorMessage(R"({"meta": {"status": 401, "msg": "Unauthorized"}})") == "Unauthorized");
}

void testKlipyResponse() {
    const QByteArray json = R"({
        "result": true,
        "data": {
            "data": [
                {"type": "gif", "title": "Hello", "file": {
                    "md": {"gif": {"url": "https://static.klipy.com/ii/a/md.gif", "width": 300, "height": 200}},
                    "sm": {"gif": {"url": "https://static.klipy.com/ii/a/sm.gif", "width": 150, "height": 100}}}},
                {"type": "ad", "file": {
                    "md": {"gif": {"url": "https://static.klipy.com/ii/ad/md.gif"}}}},
                {"title": "No type", "file": {
                    "hd": {"gif": {"url": "https://static.klipy.com/ii/b/hd.gif", "width": 640, "height": 480}}}}
            ],
            "current_page": 1, "per_page": 24, "has_next": true
        }
    })";
    const std::optional<GifPage> page = parseKlipyResponse(json);
    CHECK(page.has_value());
    CHECK(page->results.size() == 2);
    CHECK(page->results[0].previewUrl == QUrl("https://static.klipy.com/ii/a/sm.gif"));
    CHECK(page->results[0].shareUrl == QUrl("https://static.klipy.com/ii/a/md.gif"));
    CHECK(page->results[0].previewSize == QSize(150, 100));
    CHECK(page->results[1].previewUrl == QUrl("https://static.klipy.com/ii/b/hd.gif"));
    CHECK(page->hasMore);

    const QByteArray invalid =
        R"({"result":false,"errors":{"message":["The provided API key is invalid."]}})";
    CHECK(!parseKlipyResponse(invalid));
    CHECK(gifErrorMessage(invalid) == "The provided API key is invalid.");
}

void testAssembler() {
    QByteArray image(2 * kChunk + 100, '\0');
    for (qsizetype i = 0; i < image.size(); ++i) {
        image[i] = static_cast<char>(i * 7);
    }
    const QList<QByteArray> pieces = splitImage(image);
    CHECK(pieces.size() == 3);
    CHECK(pieces[0].size() == kChunk);
    CHECK(pieces[2].size() == 100);

    // Chunks may arrive in any order; duplicates are ignored.
    ImageAssembler assembler;
    CHECK(!assembler.add("bob", 1, 2, 3, pieces[2], 0));
    CHECK(!assembler.add("bob", 1, 0, 3, pieces[0], 0));
    CHECK(!assembler.add("bob", 1, 0, 3, pieces[0], 0));
    const std::optional<QByteArray> done = assembler.add("bob", 1, 1, 3, pieces[1], 0);
    CHECK(done && *done == image);
    CHECK(assembler.bufferedBytes() == 0);

    // Only the last chunk may be short, which bounds every image's size.
    CHECK(!assembler.add("bob", 2, 0, 2, pieces[2], 0));
    CHECK(assembler.bufferedBytes() == 0);
    CHECK(!assembler.add("bob", 2, 0, 2, QByteArray(kChunk + 1, 'x'), 0));
    CHECK(!assembler.add("bob", 2, 2, 2, pieces[2], 0));
    CHECK(!assembler.add("bob", 2, 0, chat::kMaxImageChunks + 1, pieces[0], 0));

    // A sender keeps at most two transfers in progress.
    CHECK(!assembler.add("eve", 10, 0, 2, pieces[0], 0));
    CHECK(!assembler.add("eve", 11, 0, 2, pieces[0], 1));
    CHECK(!assembler.add("eve", 12, 0, 2, pieces[0], 2));
    CHECK(assembler.bufferedBytes() == 2 * kChunk);
    // 10 was dropped, so this restarts it and drops 11, the oldest.
    CHECK(!assembler.add("eve", 10, 1, 2, pieces[2], 3));
    CHECK(assembler.bufferedBytes() == kChunk + 100);
    CHECK(assembler.add("eve", 12, 1, 2, pieces[2], 3));
    CHECK(assembler.bufferedBytes() == 100);

    // Transfers expire, and leaving drops a sender's transfers.
    CHECK(!assembler.add("eve", 20, 0, 2, pieces[0], 0));
    CHECK(!assembler.add("bob", 21, 0, 2, pieces[0], ImageAssembler::kTransferTimeoutMs + 10));
    CHECK(assembler.bufferedBytes() == kChunk);
    assembler.remove("bob");
    CHECK(assembler.bufferedBytes() == 0);

    // Buffered bytes are capped across all senders.
    const quint32 count = chat::kMaxImageChunks;
    const qint64 perTransfer = static_cast<qint64>(count - 1) * kChunk;
    int sender = 0;
    while (assembler.bufferedBytes() + perTransfer <= ImageAssembler::kMaxBufferedBytes) {
        const QString name = "sender" + QString::number(sender++);
        for (quint32 index = 0; index + 1 < count; ++index) {
            CHECK(!assembler.add(name, 1, index, count, pieces[0], 0));
        }
    }
    std::optional<QByteArray> late;
    for (quint32 index = 0; index < count; ++index) {
        late = assembler.add("late", 1, index, count, index + 1 < count ? pieces[0] : pieces[2], 0);
        CHECK(assembler.bufferedBytes() <= ImageAssembler::kMaxBufferedBytes);
    }
    CHECK(!late);
}

void testPrepareImage() {
    const QByteArray png = encodeImage(noise(64, 64, QImage::Format_ARGB32), "PNG");
    PreparedImage prepared = prepareImageBytes(png);
    CHECK(prepared.error.isEmpty() && prepared.bytes == png);
    prepared = prepareImageBytes(kTinyGif);
    CHECK(prepared.error.isEmpty() && prepared.bytes == kTinyGif);
    CHECK(isDisplayableImage(kTinyGif));

    CHECK(!prepareImageBytes("not an image").error.isEmpty());
    CHECK(!isDisplayableImage("not an image"));
    // The header alone reveals an image too wide to accept.
    const QByteArray wide = encodeImage(QImage(kMaxImageSide + 1, 1, QImage::Format_RGB32), "PNG");
    CHECK(!isDisplayableImage(wide));
    CHECK(!prepareImageBytes(wide).error.isEmpty());

    // Opaque images too large as PNG become JPEG.
    const QByteArray photo = encodeImage(noise(1800, 1400, QImage::Format_RGB32), "PNG");
    CHECK(photo.size() > static_cast<qsizetype>(chat::kMaxImageBytes));
    prepared = prepareImageBytes(photo);
    CHECK(prepared.error.isEmpty());
    CHECK(prepared.bytes.size() <= static_cast<qsizetype>(chat::kMaxImageBytes));
    CHECK(formatOf(prepared.bytes) == "jpeg");

    // Transparent ones stay PNG and shrink until they fit.
    prepared = prepareImage(noise(1800, 1400, QImage::Format_ARGB32));
    CHECK(prepared.error.isEmpty());
    CHECK(prepared.bytes.size() <= static_cast<qsizetype>(chat::kMaxImageBytes));
    CHECK(formatOf(prepared.bytes) == "png");
    CHECK(isDisplayableImage(prepared.bytes));
}

// Pacing keeps a socket's write buffer near the high-water mark, and every
// frame still arrives once, in order.
void testFramePacer() {
    QTcpServer server;
    CHECK(server.listen(QHostAddress::LocalHost, 0));
    QTcpSocket sender;
    sender.connectToHost(QHostAddress::LocalHost, server.serverPort());
    CHECK(sender.waitForConnected(3000));
    CHECK(server.waitForNewConnection(3000));
    QTcpSocket* receiver = server.nextPendingConnection();

    auto frames = std::make_shared<QList<QByteArray>>();
    QByteArray expected;
    for (int i = 0; i < 200; ++i) {
        frames->append(QByteArray(kChunk, static_cast<char>(i)));
        expected += frames->back();
    }
    FramePacer pacer;
    pacer.enqueue(&sender, frames);
    CHECK(sender.bytesToWrite() < FramePacer::kHighWaterBytes + kChunk);

    QByteArray received;
    qint64 largestBacklog = 0;
    QElapsedTimer timer;
    timer.start();
    while (received.size() < expected.size() && timer.elapsed() < 20000) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        largestBacklog = std::max(largestBacklog, sender.bytesToWrite());
        received += receiver->readAll();
    }
    CHECK(received == expected);
    CHECK(largestBacklog < FramePacer::kHighWaterBytes + kChunk);
}

} // namespace

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    testMediaLinks();
    testRequestUrls();
    testGiphyResponse();
    testKlipyResponse();
    testAssembler();
    testPrepareImage();
    testFramePacer();
    std::puts("desktop_media_test: ok");
    return 0;
}
