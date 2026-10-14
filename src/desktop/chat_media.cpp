#include "desktop/chat_media.h"

#include <QBuffer>
#include <QDesktopServices>
#include <QDialog>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QMovie>
#include <QPainter>
#include <QPixmap>
#include <QPushButton>
#include <QScreen>
#include <QScrollBar>
#include <QTextBlock>
#include <QTextBrowser>
#include <QTextDocument>
#include <QVBoxLayout>

#include "desktop/image_transfer.h"
#include "desktop/remote_media.h"

namespace {

// Scales `size` down, never up, to fit within `bounds`.
QSize fitWithin(QSize size, const QSize& bounds) {
    if (size.isEmpty()) {
        return {};
    }
    if (size.width() > bounds.width() || size.height() > bounds.height()) {
        size.scale(bounds, Qt::KeepAspectRatio);
    }
    return size.expandedTo(QSize(1, 1));
}

// A muted box standing in for media that is loading or failed to load.
QImage notice(const QString& text, const QSize& size, qreal devicePixelRatio) {
    QImage image(size * devicePixelRatio, QImage::Format_ARGB32_Premultiplied);
    image.setDevicePixelRatio(devicePixelRatio);
    image.fill(QColor("#333842"));
    QPainter painter(&image);
    painter.setPen(QColor("#8a9099"));
    painter.drawText(QRect(QPoint(0, 0), size), Qt::AlignCenter, text);
    return image;
}

void saveImage(QWidget* parent, const QByteArray& bytes) {
    QByteArray format = ImageBytes(bytes).format();
    if (format == "jpeg") {
        format = "jpg";
    }
    const QString path = QFileDialog::getSaveFileName(
        parent, "Save image", QDir::homePath() + "/image." + QString::fromLatin1(format));
    if (path.isEmpty()) {
        return;
    }
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size()) {
        QMessageBox::warning(parent, "Save image",
                             "Could not save the image: " + file.errorString());
    }
}

// Shows an uploaded image as large as the screen allows, with a save button.
void openViewer(QWidget* parent, const QByteArray& bytes) {
    auto* dialog = new QDialog(parent);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setWindowTitle("image");
    auto* layout = new QVBoxLayout(dialog);
    auto* label = new QLabel(dialog);
    label->setAlignment(Qt::AlignCenter);
    layout->addWidget(label, 1);

    ImageBytes image(bytes);
    const QScreen* screen = parent->screen();
    const QSize available = screen != nullptr ? screen->availableSize() * 4 / 5 : QSize(1280, 800);
    const QSize size = fitWithin(image.size(), available);
    const qreal ratio = parent->devicePixelRatioF();
    if (image.isAnimated()) {
        if (QMovie* movie = movieFromBytes(bytes).release()) {
            movie->setParent(dialog);
            movie->setScaledSize(size * ratio);
            label->setMovie(movie);
            movie->start();
        }
    }
    else {
        QPixmap pixmap = QPixmap::fromImage(
            image.read().scaled(size * ratio, Qt::IgnoreAspectRatio, Qt::SmoothTransformation));
        pixmap.setDevicePixelRatio(ratio);
        label->setPixmap(pixmap);
    }

    auto* buttons = new QHBoxLayout;
    auto* save = new QPushButton("save…", dialog);
    auto* close = new QPushButton("close", dialog);
    buttons->addStretch();
    buttons->addWidget(save);
    buttons->addWidget(close);
    layout->addLayout(buttons);
    QObject::connect(save, &QPushButton::clicked, dialog,
                     [dialog, bytes] { saveImage(dialog, bytes); });
    QObject::connect(close, &QPushButton::clicked, dialog, &QDialog::close);
    dialog->show();
}

} // namespace

std::unique_ptr<QMovie> movieFromBytes(const QByteArray& bytes) {
    auto movie = std::make_unique<QMovie>();
    auto* buffer = new QBuffer(movie.get());
    buffer->setData(bytes);
    buffer->open(QIODevice::ReadOnly);
    movie->setDevice(buffer);
    if (!movie->isValid()) {
        return nullptr;
    }
    return movie;
}

ChatMedia::ChatMedia(QTextBrowser* view, MediaFetcher* fetcher, QObject* parent) :
    QObject(parent), view_(view), fetcher_(fetcher) {
    view_->setOpenLinks(false);
    connect(view_, &QTextBrowser::anchorClicked, this, &ChatMedia::open);
    // Media finishes loading after its message was appended. Keep following
    // the newest message unless the reader has scrolled up.
    QScrollBar* bar = view_->verticalScrollBar();
    connect(bar, &QScrollBar::valueChanged, this,
            [this, bar](int value) { pinnedToBottom_ = value >= bar->maximum() - 4; });
    connect(bar, &QScrollBar::rangeChanged, this, [this, bar](int, int maximum) {
        if (pinnedToBottom_) {
            bar->setValue(maximum);
        }
    });
}

ChatMedia::~ChatMedia() = default;

QUrl ChatMedia::resourceUrl(int id) {
    return QUrl("embed:" + QString::number(id));
}

void ChatMedia::appendRemote(const QUrl& url) {
    Embed embed;
    embed.source = url;
    const int id = insert(std::move(embed),
                          notice("loading…", QSize(160, 90), view_->devicePixelRatioF()));
    fetcher_->fetch(url, this, [this, id](const QByteArray& bytes) {
        if (!embeds_.contains(id)) {
            return;
        }
        if (bytes.isEmpty() || !isDisplayableImage(bytes)) {
            setFrame(id, notice("GIF unavailable", QSize(160, 40), view_->devicePixelRatioF()));
            return;
        }
        show(id, bytes);
    });
}

bool ChatMedia::appendImage(const QByteArray& bytes) {
    if (!isDisplayableImage(bytes)) {
        return false;
    }
    Embed embed;
    embed.bytes = bytes;
    QImage placeholder(1, 1, QImage::Format_ARGB32_Premultiplied);
    placeholder.fill(Qt::transparent);
    show(insert(std::move(embed), placeholder), bytes);
    return true;
}

void ChatMedia::clear() {
    embeds_.clear();
    playing_.clear();
}

int ChatMedia::insert(Embed embed, const QImage& placeholder) {
    const int id = nextId_++;
    embed.shownSize = placeholder.deviceIndependentSize().toSize();
    view_->document()->addResource(QTextDocument::ImageResource, resourceUrl(id),
                                   QPixmap::fromImage(placeholder));
    const QString name = resourceUrl(id).toString();
    view_->append("<a href=\"" + name + "\"><img src=\"" + name + "\"></a>");
    // The transcript only grows at its end, so this position stays valid.
    embed.position = view_->document()->lastBlock().position();
    embeds_.emplace(id, std::move(embed));
    return id;
}

void ChatMedia::show(int id, const QByteArray& bytes) {
    const auto found = embeds_.find(id);
    if (found == embeds_.end()) {
        return;
    }
    Embed& embed = found->second;
    ImageBytes image(bytes);
    embed.displaySize = fitWithin(image.size(), QSize(kMaxWidth, kMaxHeight));
    if (image.isAnimated()) {
        if (std::unique_ptr<QMovie> movie = movieFromBytes(bytes)) {
            // Decoding straight to the display size is cheaper than scaling
            // every frame afterwards.
            movie->setScaledSize(embed.displaySize * view_->devicePixelRatioF());
            connect(movie.get(), &QMovie::frameChanged, this, [this, id] {
                const auto playing = embeds_.find(id);
                if (playing != embeds_.end() && playing->second.movie) {
                    setFrame(id, playing->second.movie->currentImage());
                }
            });
            embed.movie = std::move(movie);
            startPlaying(id);
            return;
        }
    }
    setFrame(id, image.read());
}

void ChatMedia::setFrame(int id, const QImage& frame) {
    const auto found = embeds_.find(id);
    if (found == embeds_.end() || frame.isNull()) {
        return;
    }
    Embed& embed = found->second;
    QImage image = frame;
    if (embed.displaySize.isValid()) {
        const qreal ratio = view_->devicePixelRatioF();
        const QSize target = embed.displaySize * ratio;
        if (image.size() != target) {
            image = image.scaled(target, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
        }
        image.setDevicePixelRatio(ratio);
    }
    QTextDocument* document = view_->document();
    document->addResource(QTextDocument::ImageResource, resourceUrl(id), QPixmap::fromImage(image));
    const QSize shown = image.deviceIndependentSize().toSize();
    if (shown != embed.shownSize) {
        embed.shownSize = shown;
        document->markContentsDirty(embed.position, 1);
    }
    else {
        view_->viewport()->update();
    }
}

void ChatMedia::startPlaying(int id) {
    embeds_.at(id).movie->start();
    playing_.push_back(id);
    // Older embeds keep their current frame.
    while (playing_.size() > kMaxPlaying) {
        const auto oldest = embeds_.find(playing_.front());
        playing_.pop_front();
        if (oldest != embeds_.end()) {
            oldest->second.movie.reset();
        }
    }
}

void ChatMedia::open(const QUrl& link) {
    bool valid = false;
    const int id = link.scheme() == "embed" ? link.path().toInt(&valid) : 0;
    const auto found = embeds_.find(id);
    if (!valid || found == embeds_.end()) {
        return;
    }
    if (!found->second.source.isEmpty()) {
        QDesktopServices::openUrl(found->second.source);
    }
    else {
        openViewer(view_->window(), found->second.bytes);
    }
}
