#include "desktop/screen_stage.h"

#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPainter>
#include <QPushButton>
#include <QShortcut>
#include <QStackedWidget>
#include <QVBoxLayout>

#include <cmath>

StreamView::StreamView(QWidget* parent) : QWidget(parent) {
    setMinimumSize(160, 90);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
}

void StreamView::setImage(const QImage& image) {
    image_ = image;
    update();
}

void StreamView::clear() {
    image_ = QImage();
    update();
}

void StreamView::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.fillRect(rect(), QColor("#16181d"));
    if (image_.isNull()) {
        painter.setPen(QColor("#70757c"));
        painter.drawText(rect(), Qt::AlignCenter, "waiting for video");
        return;
    }
    // Drawing in logical pixels lets Qt paint at full device resolution.
    const QSize fitted = image_.size().scaled(size(), Qt::KeepAspectRatio);
    const QRect target(QPoint((width() - fitted.width()) / 2, (height() - fitted.height()) / 2),
                       fitted);
    painter.setRenderHint(QPainter::SmoothPixmapTransform);
    painter.drawImage(target, image_);
}

void StreamView::mouseDoubleClickEvent(QMouseEvent*) {
    emit doubleClicked();
}

StreamTile::StreamTile(const QString& name, bool own, QWidget* parent)
    : QFrame(parent), name_(name), own_(own) {
    setObjectName("streamTile");
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);

    auto* title = new QLabel(own ? name + " (you)" : name, this);
    title->setObjectName("streamName");
    auto* live = new QLabel("LIVE", this);
    live->setObjectName("live");
    stats_ = new QLabel(this);
    stats_->setObjectName("streamStats");
    // Lets the statistics give way first when the tile is narrow.
    stats_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    stats_->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    focusButton_ = new QPushButton("focus", this);
    fullscreenButton_ = new QPushButton("fullscreen", this);
    stopButton_ = new QPushButton("stop watching", this);

    auto* header = new QHBoxLayout;
    header->setSpacing(6);
    header->addWidget(title);
    header->addWidget(live);
    header->addWidget(stats_, 1);
    header->addWidget(focusButton_);
    header->addWidget(fullscreenButton_);
    header->addWidget(stopButton_);

    invitation_ = new QWidget(this);
    auto* inviteText = new QLabel(name + " is sharing their screen", invitation_);
    inviteText->setAlignment(Qt::AlignCenter);
    inviteText->setWordWrap(true);
    auto* watchButton = new QPushButton("watch stream", invitation_);
    watchButton->setObjectName("primary");
    auto* invitationLayout = new QVBoxLayout(invitation_);
    invitationLayout->addStretch();
    invitationLayout->addWidget(inviteText);
    invitationLayout->addWidget(watchButton, 0, Qt::AlignHCenter);
    invitationLayout->addStretch();

    view_ = new StreamView(this);
    body_ = new QStackedWidget(this);
    body_->addWidget(invitation_);
    body_->addWidget(view_);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(6, 6, 6, 6);
    layout->setSpacing(6);
    layout->addLayout(header);
    layout->addWidget(body_, 1);

    connect(watchButton, &QPushButton::clicked, this, &StreamTile::watchClicked);
    connect(stopButton_, &QPushButton::clicked, this, &StreamTile::stopWatchingClicked);
    connect(focusButton_, &QPushButton::clicked, this, &StreamTile::focusClicked);
    connect(fullscreenButton_, &QPushButton::clicked, this, &StreamTile::fullscreenClicked);
    connect(view_, &StreamView::doubleClicked, this, &StreamTile::fullscreenClicked);

    setWatching(own);
}

void StreamTile::setWatching(bool watching) {
    body_->setCurrentWidget(watching ? static_cast<QWidget*>(view_) : invitation_);
    fullscreenButton_->setVisible(watching);
    stopButton_->setVisible(watching && !own_);
    view_->clear();
    stats_->clear();
    statsClock_.invalidate();
    statsFrames_ = 0;
    statsBytes_ = 0;
}

void StreamTile::setFocusState(bool available, bool focused) {
    focusButton_->setVisible(available);
    focusButton_->setText(focused ? "show all" : "focus");
}

void StreamTile::showFrame(const QImage& image, qsizetype encodedBytes) {
    view_->setImage(image);
    if (!statsClock_.isValid()) {
        statsClock_.start();
    }
    ++statsFrames_;
    statsBytes_ += encodedBytes;

    const qint64 elapsed = statsClock_.elapsed();
    if (elapsed < 1000) {
        return;
    }
    QStringList parts;
    if (!detail_.isEmpty()) {
        parts << detail_;
    }
    parts << QString("%1x%2").arg(image.width()).arg(image.height())
          << QString("%1 fps").arg(statsFrames_ * 1000.0 / static_cast<double>(elapsed), 0, 'f', 0)
          << QString("%1 Mbps").arg(static_cast<double>(statsBytes_) * 8.0 / 1000.0 /
                                        static_cast<double>(elapsed),
                                    0, 'f', 1);
    stats_->setText(parts.join(", "));
    statsClock_.restart();
    statsFrames_ = 0;
    statsBytes_ = 0;
}

ScreenStage::ScreenStage(QWidget* parent) : QWidget(parent) {
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    relayout();
}

void ScreenStage::addStream(const QString& name, bool own) {
    if (find(name)) {
        return;
    }
    auto* tile = new StreamTile(name, own, this);
    connect(tile, &StreamTile::watchClicked, this, [this, name] { emit watchRequested(name); });
    connect(tile, &StreamTile::stopWatchingClicked, this,
            [this, name] { emit stopWatchingRequested(name); });
    connect(tile, &StreamTile::focusClicked, this, [this, name] { toggleFocus(name); });
    connect(tile, &StreamTile::fullscreenClicked, this, [this, name] { toggleFullscreen(name); });
    // Your own share comes first, like in a call.
    if (own) {
        tiles_.prepend(tile);
    }
    else {
        tiles_.append(tile);
    }
    relayout();
}

void ScreenStage::removeStream(const QString& name) {
    StreamTile* tile = find(name);
    if (!tile) {
        return;
    }
    if (name == fullscreenName_) {
        exitFullscreen();
    }
    tiles_.removeOne(tile);
    tile->hide();
    tile->deleteLater();
    relayout();
}

void ScreenStage::setWatching(const QString& name, bool watching) {
    if (StreamTile* tile = find(name)) {
        tile->setWatching(watching);
    }
    if (!watching && name == fullscreenName_) {
        exitFullscreen();
    }
}

void ScreenStage::setDetail(const QString& name, const QString& detail) {
    if (StreamTile* tile = find(name)) {
        tile->setDetail(detail);
    }
}

void ScreenStage::showFrame(const QString& name, const QImage& image, qsizetype encodedBytes) {
    if (StreamTile* tile = find(name)) {
        tile->showFrame(image, encodedBytes);
    }
    if (fullscreen_ && name == fullscreenName_) {
        fullscreen_->setImage(image);
    }
}

StreamTile* ScreenStage::find(const QString& name) const {
    for (StreamTile* tile : tiles_) {
        if (tile->name() == name) {
            return tile;
        }
    }
    return nullptr;
}

void ScreenStage::toggleFocus(const QString& name) {
    focused_ = focused_ == name ? QString() : name;
    relayout();
}

void ScreenStage::toggleFullscreen(const QString& name) {
    const bool wasThisStream = fullscreen_ && name == fullscreenName_;
    exitFullscreen();
    StreamTile* tile = find(name);
    if (wasThisStream || !tile) {
        return;
    }
    // A child window, so it closes along with the app.
    fullscreen_ = new StreamView(this);
    fullscreen_->setWindowFlag(Qt::Window);
    fullscreen_->setAttribute(Qt::WA_DeleteOnClose);
    fullscreen_->setWindowTitle(name + "'s screen");
    fullscreen_->setImage(tile->image());
    fullscreenName_ = name;
    connect(fullscreen_, &StreamView::doubleClicked, this, &ScreenStage::exitFullscreen);
    auto* escape = new QShortcut(Qt::Key_Escape, fullscreen_);
    connect(escape, &QShortcut::activated, this, &ScreenStage::exitFullscreen);
    fullscreen_->showFullScreen();
    fullscreen_->activateWindow();
}

void ScreenStage::exitFullscreen() {
    if (fullscreen_) {
        fullscreen_->close();
    }
    fullscreenName_.clear();
}

void ScreenStage::relayout() {
    // A fresh grid is simpler than undoing the old one's spans and stretches.
    // Deleting a layout leaves its widgets alone.
    delete layout();
    auto* grid = new QGridLayout(this);
    grid->setContentsMargins(0, 0, 0, 6);
    grid->setSpacing(6);

    const bool canFocus = tiles_.size() > 1;
    StreamTile* focused = canFocus ? find(focused_) : nullptr;
    if (!focused) {
        focused_.clear();
    }

    if (focused) {
        // The focused tile fills the stage, with the others in a strip below.
        const auto stripSize = static_cast<int>(tiles_.size()) - 1;
        grid->addWidget(focused, 0, 0, 1, stripSize);
        int column = 0;
        for (StreamTile* tile : std::as_const(tiles_)) {
            if (tile != focused) {
                grid->addWidget(tile, 1, column++);
            }
        }
        grid->setRowStretch(0, 4);
        grid->setRowStretch(1, 1);
    }
    else {
        const int columns = static_cast<int>(std::ceil(std::sqrt(tiles_.size())));
        for (int i = 0; i < tiles_.size(); ++i) {
            grid->addWidget(tiles_[i], i / columns, i % columns);
        }
    }

    for (StreamTile* tile : std::as_const(tiles_)) {
        tile->setFocusState(canFocus, tile == focused);
    }
    const bool opening = isHidden() && !tiles_.isEmpty();
    setVisible(!tiles_.isEmpty());
    if (opening) {
        emit opened();
    }
}
