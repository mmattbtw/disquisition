#include "desktop/gif_picker.h"

#include <QGuiApplication>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMovie>
#include <QPixmap>
#include <QScreen>
#include <QScrollBar>
#include <QTabBar>
#include <QVBoxLayout>
#include <algorithm>

#include "desktop/chat_media.h"
#include "desktop/remote_media.h"

namespace {

const QSize kTileSize(168, 120);
constexpr int kTileSpacing = 6;
constexpr int kSearchDelayMs = 350;
// Start loading the next page this close to the bottom.
constexpr int kLoadMoreMarginPx = 200;
constexpr int kUrlRole = Qt::UserRole;

QPixmap blankTile(qreal ratio) {
    QPixmap pixmap(kTileSize * ratio);
    pixmap.setDevicePixelRatio(ratio);
    pixmap.fill(QColor("#333842"));
    return pixmap;
}

} // namespace

GifPicker::GifPicker(GifSearch* search, MediaFetcher* fetcher, QWidget* parent) :
    QFrame(parent, Qt::Popup), search_(search), fetcher_(fetcher) {
    setObjectName("gifPicker");
    resize(4 * (kTileSize.width() + kTileSpacing) + 40, 460);
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(8, 8, 8, 8);
    layout->setSpacing(6);

    providers_ = new QTabBar(this);
    providers_->setExpanding(false);
    providers_->setDrawBase(false);
    query_ = new QLineEdit(this);
    query_->setPlaceholderText("search GIFs");
    query_->setClearButtonEnabled(true);
    status_ = new QLabel(this);
    status_->setObjectName("gifStatus");
    status_->setWordWrap(true);
    status_->hide();
    results_ = new QListWidget(this);
    results_->setViewMode(QListView::IconMode);
    results_->setIconSize(kTileSize);
    results_->setGridSize(kTileSize + QSize(kTileSpacing, kTileSpacing));
    results_->setResizeMode(QListView::Adjust);
    results_->setMovement(QListView::Static);
    results_->setUniformItemSizes(true);
    results_->setSelectionMode(QAbstractItemView::SingleSelection);
    results_->setMouseTracking(true);
    attribution_ = new QLabel(this);
    attribution_->setObjectName("gifAttribution");
    attribution_->setAlignment(Qt::AlignRight);

    layout->addWidget(providers_);
    layout->addWidget(query_);
    layout->addWidget(status_);
    layout->addWidget(results_, 1);
    layout->addWidget(attribution_);

    debounce_.setSingleShot(true);
    debounce_.setInterval(kSearchDelayMs);
    connect(&debounce_, &QTimer::timeout, this, &GifPicker::startSearch);
    connect(query_, &QLineEdit::textEdited, &debounce_, qOverload<>(&QTimer::start));
    connect(query_, &QLineEdit::returnPressed, this, [this] {
        debounce_.stop();
        if (query_->text().trimmed() != shownQuery_) {
            startSearch();
        }
        else if (results_->count() > 0) {
            choose(results_->item(0)->data(kUrlRole).toUrl());
        }
    });
    connect(providers_, &QTabBar::currentChanged, this, [this] { startSearch(); });
    connect(results_, &QListWidget::itemClicked, this,
            [this](QListWidgetItem* item) { choose(item->data(kUrlRole).toUrl()); });
    connect(results_, &QListWidget::itemActivated, this,
            [this](QListWidgetItem* item) { choose(item->data(kUrlRole).toUrl()); });
    connect(results_->verticalScrollBar(), &QScrollBar::valueChanged, this, [this](int value) {
        updatePlayback();
        if (value >= results_->verticalScrollBar()->maximum() - kLoadMoreMarginPx) {
            loadMore();
        }
    });
    connect(results_->verticalScrollBar(), &QScrollBar::rangeChanged, this,
            [this] { updatePlayback(); });
    connect(search_, &GifSearch::pageReady, this, &GifPicker::showPage);
    connect(search_, &GifSearch::failed, this, &GifPicker::showFailure);
}

GifPicker::~GifPicker() = default;

void GifPicker::popup(QWidget* anchor, const QString& query) {
    const bool providersChanged = refreshProviders();
    const QString trimmed = query.trimmed();
    const bool queryChanged = trimmed != query_->text().trimmed();
    if (queryChanged) {
        query_->setText(trimmed);
    }
    if (providersChanged || queryChanged || (results_->count() == 0 && !loading_)) {
        startSearch();
    }

    // Open above the anchor, right-aligned with it, kept on screen. On a
    // screen smaller than the picker, keep its top left corner visible.
    QPoint position = anchor->mapToGlobal(QPoint(anchor->width() - width(), -height() - 6));
    if (const QScreen* screen = anchor->screen()) {
        const QRect area = screen->availableGeometry();
        position.setX(std::max(area.left(), std::min(position.x(), area.right() - width())));
        position.setY(std::max(area.top(), std::min(position.y(), area.bottom() - height())));
    }
    move(position);
    show();
    raise();
    query_->setFocus();
    query_->selectAll();
}

void GifPicker::showEvent(QShowEvent* event) {
    QFrame::showEvent(event);
    updatePlayback();
}

void GifPicker::hideEvent(QHideEvent* event) {
    debounce_.stop();
    for (const Preview& preview : previews_) {
        if (preview.movie->state() == QMovie::Running) {
            preview.movie->setPaused(true);
        }
    }
    QFrame::hideEvent(event);
}

void GifPicker::keyPressEvent(QKeyEvent* event) {
    if (event->key() == Qt::Key_Escape) {
        hide();
        return;
    }
    QFrame::keyPressEvent(event);
}

bool GifPicker::refreshProviders() {
    QList<GifProvider> available;
    for (const GifProvider candidate : {GifProvider::Giphy, GifProvider::Klipy}) {
        if (search_->hasKey(candidate)) {
            available.append(candidate);
        }
    }
    QList<GifProvider> shown;
    for (int i = 0; i < providers_->count(); ++i) {
        shown.append(static_cast<GifProvider>(providers_->tabData(i).toInt()));
    }
    if (available == shown) {
        return false;
    }

    const QSignalBlocker blocker(providers_);
    const GifProvider previous = providers_->count() > 0 ? provider() : GifProvider::Giphy;
    while (providers_->count() > 0) {
        providers_->removeTab(0);
    }
    for (const GifProvider candidate : available) {
        const int index = providers_->addTab(gifProviderName(candidate));
        providers_->setTabData(index, static_cast<int>(candidate));
        if (candidate == previous) {
            providers_->setCurrentIndex(index);
        }
    }
    providers_->setVisible(available.size() > 1);
    query_->setEnabled(!available.isEmpty());
    return true;
}

GifProvider GifPicker::provider() const {
    return static_cast<GifProvider>(providers_->tabData(providers_->currentIndex()).toInt());
}

void GifPicker::startSearch() {
    debounce_.stop();
    ++generation_;
    previews_.clear();
    results_->clear();
    page_ = 0;
    hasMore_ = false;
    shownQuery_ = query_->text().trimmed();
    if (providers_->count() == 0) {
        loading_ = false;
        search_->cancel();
        attribution_->clear();
        setStatus("GIF search is not available in this build.");
        return;
    }
    const QString name = gifProviderName(provider());
    query_->setPlaceholderText("search " + name);
    attribution_->setText("Powered by " + name);
    loading_ = true;
    requestedPage_ = 0;
    setStatus("loading…");
    search_->request(provider(), shownQuery_, 0);
}

void GifPicker::loadMore() {
    if (!hasMore_ || loading_ || providers_->count() == 0) {
        return;
    }
    loading_ = true;
    requestedPage_ = page_ + 1;
    search_->request(provider(), shownQuery_, requestedPage_);
}

void GifPicker::showPage(GifProvider source, const QString& query, int page,
                         const GifPage& results) {
    if (!loading_ || providers_->count() == 0 || source != provider() || query != shownQuery_ ||
        page != requestedPage_) {
        return;
    }
    loading_ = false;
    page_ = page;
    hasMore_ = results.hasMore;
    setStatus(page == 0 && results.results.isEmpty() ? "No GIFs found." : QString());

    const qreal ratio = devicePixelRatioF();
    const QPixmap blank = blankTile(ratio);
    for (const GifResult& result : results.results) {
        auto* item = new QListWidgetItem(QIcon(blank), QString(), results_);
        item->setData(kUrlRole, result.shareUrl);
        item->setToolTip(result.title);
        item->setSizeHint(kTileSize);
        const QSize size = result.previewSize.isValid()
                               ? result.previewSize.scaled(kTileSize, Qt::KeepAspectRatio)
                               : kTileSize;
        const quint64 generation = generation_;
        fetcher_->fetch(result.previewUrl, this,
                        [this, item, generation, size, ratio](const QByteArray& bytes) {
                            // Items outlive their generation's previews, so `item` is valid
                            // while the generation matches.
                            if (generation != generation_) {
                                return;
                            }
                            std::unique_ptr<QMovie> movie = bytes.isEmpty() ? nullptr
                                                                            : movieFromBytes(bytes);
                            if (!movie) {
                                delete item; // removed or unreachable GIF; leave no gap
                                return;
                            }
                            movie->setScaledSize(size * ratio);
                            QMovie* raw = movie.get();
                            connect(raw, &QMovie::frameChanged, this, [item, raw, ratio] {
                                QPixmap frame = QPixmap::fromImage(raw->currentImage());
                                frame.setDevicePixelRatio(ratio);
                                item->setIcon(QIcon(frame));
                            });
                            raw->start();
                            previews_.push_back({item, std::move(movie)});
                            updatePlayback(previews_.back());
                        });
    }
    // A short first page may not fill the view, so there is nothing to scroll.
    QScrollBar* bar = results_->verticalScrollBar();
    if (hasMore_ && bar->maximum() - bar->value() < kLoadMoreMarginPx) {
        loadMore();
    }
}

void GifPicker::showFailure(GifProvider source, const QString& query, int page,
                            const QString& message) {
    if (!loading_ || providers_->count() == 0 || source != provider() || query != shownQuery_ ||
        page != requestedPage_) {
        return;
    }
    loading_ = false;
    hasMore_ = false;
    setStatus(message);
}

void GifPicker::setStatus(const QString& text) {
    status_->setText(text);
    status_->setVisible(!text.isEmpty());
}

// Every page scrolled past keeps its previews, and animating them all makes
// scrolling stutter. Those out of view keep their current frame.
void GifPicker::updatePlayback() {
    for (const Preview& preview : previews_) {
        updatePlayback(preview);
    }
}

void GifPicker::updatePlayback(const Preview& preview) {
    const bool onScreen =
        isVisible() &&
        results_->visualItemRect(preview.item).intersects(results_->viewport()->rect());
    if (onScreen && preview.movie->state() == QMovie::Paused) {
        preview.movie->setPaused(false);
    }
    else if (!onScreen && preview.movie->state() == QMovie::Running) {
        preview.movie->setPaused(true);
    }
}

void GifPicker::choose(const QUrl& gif) {
    // A click can report both clicked and activated.
    if (!isVisible() || !gif.isValid()) {
        return;
    }
    hide();
    emit chosen(gif);
}
