#include "desktop/screen_stage.h"

#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPainter>
#include <QPushButton>
#include <QShortcut>
#include <QContextMenuEvent>
#include <QMenu>
#include <QSlider>
#include <QWidgetAction>
#include <QStackedWidget>
#include <QVBoxLayout>

#include <cmath>

StreamView::StreamView(QWidget* parent) : DrawingCanvas(parent) {
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

QRectF StreamView::drawingRect() const {
    if (image_.isNull())
        return {};
    const QSize fitted = image_.size().scaled(size(), Qt::KeepAspectRatio);
    return QRectF(QPointF((width() - fitted.width()) / 2.0, (height() - fitted.height()) / 2.0),
                  fitted);
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
    const QRectF target = drawingRect();
    painter.setRenderHint(QPainter::SmoothPixmapTransform);
    painter.drawImage(target, image_);
    paintDrawing(painter);
}

void StreamView::mouseDoubleClickEvent(QMouseEvent*) {
    if (!drawing() || !drawing()->penActive())
        emit doubleClicked();
}

void StreamView::contextMenuEvent(QContextMenuEvent* event) {
    emit menuRequested(event->globalPos());
}

StreamTile::StreamTile(const QString& name, bool own, QWidget* parent, const QString& label)
    : QFrame(parent), name_(name), own_(own) {
    setObjectName("streamTile");
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);

    title_ = new QLabel(this);
    title_->setTextFormat(Qt::PlainText);
    title_->setObjectName("streamName");
    auto* live = new QLabel("LIVE", this);
    live->setObjectName("live");
    stats_ = new QLabel(this);
    stats_->setObjectName("streamStats");
    // Lets the statistics give way first when the tile is narrow.
    stats_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    stats_->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    focusButton_ = new QPushButton("Focus", this);
    fullscreenButton_ = new QPushButton("Fullscreen", this);
    stopButton_ = new QPushButton("Stop Watching", this);

    auto* header = new QHBoxLayout;
    header->setSpacing(6);
    header->addWidget(title_);
    header->addWidget(live);
    header->addWidget(stats_, 1);
    header->addWidget(focusButton_);
    header->addWidget(fullscreenButton_);
    header->addWidget(stopButton_);

    invitation_ = new QWidget(this);
    invitationText_ = new QLabel(invitation_);
    invitationText_->setTextFormat(Qt::PlainText);
    invitationText_->setAlignment(Qt::AlignCenter);
    invitationText_->setWordWrap(true);
    setDisplayName(label.isEmpty() ? name : label);
    auto* watchButton = new QPushButton("Watch Stream", invitation_);
    watchButton->setObjectName("primary");
    auto* invitationLayout = new QVBoxLayout(invitation_);
    invitationLayout->addStretch();
    invitationLayout->addWidget(invitationText_);
    invitationLayout->addWidget(watchButton, 0, Qt::AlignHCenter);
    invitationLayout->addStretch();

    view_ = new StreamView(this);
    drawing_ = new ScreenDrawing(this);
    view_->setDrawing(drawing_);
    drawingControls_ = new DrawingControls(drawing_, own, this);
    body_ = new QStackedWidget(this);
    body_->addWidget(invitation_);
    body_->addWidget(view_);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(6, 6, 6, 6);
    layout->setSpacing(6);
    layout->addLayout(header);
    layout->addWidget(drawingControls_);
    layout->addWidget(body_, 1);

    connect(watchButton, &QPushButton::clicked, this, &StreamTile::watchClicked);
    connect(stopButton_, &QPushButton::clicked, this, &StreamTile::stopWatchingClicked);
    connect(focusButton_, &QPushButton::clicked, this, &StreamTile::focusClicked);
    connect(fullscreenButton_, &QPushButton::clicked, this, &StreamTile::fullscreenClicked);
    connect(view_, &StreamView::doubleClicked, this, &StreamTile::fullscreenClicked);
    connect(view_, &StreamView::menuRequested, this, &StreamTile::showVolumeMenu);

    setWatching(own);
}

void StreamTile::setDisplayName(const QString& label) {
    label_ = label;
    title_->setText(own_ ? label + " (you)" : label);
    invitationText_->setText(label + " is sharing their screen");
}

void StreamTile::setWatching(bool watching) {
    body_->setCurrentWidget(watching ? static_cast<QWidget*>(view_) : invitation_);
    fullscreenButton_->setVisible(watching);
    stopButton_->setVisible(watching && !own_);
    watching_ = watching;
    drawingControls_->setVisible(watching);
    if (!watching)
        drawing_->setState(false, 0);
    view_->clear();
    stats_->clear();
    statsClock_.invalidate();
    statsFrames_ = 0;
    statsBytes_ = 0;
}

void StreamTile::showVolumeMenu(const QPoint& globalPosition) {
    // Your own share's sound is never played back to you.
    if (own_ || !watching_)
        return;
    QMenu menu(this);
    auto* row = new QWidget(&menu);
    auto* layout = new QHBoxLayout(row);
    layout->setContentsMargins(10, 6, 10, 6);
    auto* label = new QLabel(row);
    label->setMinimumWidth(label->fontMetrics().horizontalAdvance("Volume 100%"));
    auto* slider = new QSlider(Qt::Horizontal, row);
    slider->setObjectName("streamVolume");
    slider->setRange(0, 100);
    slider->setFixedWidth(140);
    slider->setValue(volume_);
    label->setText(QString("Volume %1%").arg(volume_));
    layout->addWidget(label);
    layout->addWidget(slider);
    auto* action = new QWidgetAction(&menu);
    action->setDefaultWidget(row);
    menu.addAction(action);
    connect(slider, &QSlider::valueChanged, this, [this, label](int percent) {
        volume_ = percent;
        label->setText(QString("Volume %1%").arg(percent));
        emit volumeChanged(percent);
    });
    menu.exec(globalPosition);
}

void StreamTile::setFocusState(bool available, bool focused) {
    focusButton_->setVisible(available);
    focusButton_->setText(focused ? "Show All" : "Focus");
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
    // One shortcut covers tiles, fullscreen windows and the floating toolbar,
    // including when the chat composer held focus before pen mode started.
    escape_ = new QShortcut(Qt::Key_Escape, this);
    escape_->setContext(Qt::ApplicationShortcut);
    escape_->setEnabled(false);
    connect(escape_, &QShortcut::activated, this, [this] {
        bool hadPen = false;
        for (StreamTile* tile : std::as_const(tiles_)) {
            if (tile->drawing()->penActive()) {
                hadPen = true;
                tile->drawing()->setPenActive(false);
            }
        }
        if (!hadPen)
            exitFullscreen();
    });
    relayout();
}

void ScreenStage::refreshEscapeShortcut() {
    bool active = !fullscreen_.isNull();
    for (StreamTile* tile : std::as_const(tiles_))
        active |= tile->drawing()->penActive();
    escape_->setEnabled(active);
}

void ScreenStage::addStream(const QString& name, bool own, const QString& label) {
    if (find(name)) {
        return;
    }
    auto* tile = new StreamTile(name, own, this, label);
    connect(tile->drawing(), &ScreenDrawing::changed, this, &ScreenStage::refreshEscapeShortcut);
    connect(tile, &StreamTile::watchClicked, this, [this, name] { emit watchRequested(name); });
    connect(tile, &StreamTile::stopWatchingClicked, this,
            [this, name] { emit stopWatchingRequested(name); });
    connect(tile, &StreamTile::focusClicked, this, [this, name] { toggleFocus(name); });
    connect(tile, &StreamTile::fullscreenClicked, this, [this, name] { toggleFullscreen(name); });
    connect(tile, &StreamTile::volumeChanged, this,
            [this, name](int percent) { emit volumeChanged(name, percent); });
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
    refreshEscapeShortcut();
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

void ScreenStage::setVolume(const QString& name, int percent) {
    if (auto* tile = find(name))
        tile->setVolume(percent);
}

void ScreenStage::setDetail(const QString& name, const QString& detail) {
    if (StreamTile* tile = find(name)) {
        tile->setDetail(detail);
    }
}

void ScreenStage::setDisplayName(const QString& name, const QString& label) {
    if (auto* tile = find(name))
        tile->setDisplayName(label);
    if (fullscreen_ && fullscreenName_ == name)
        fullscreen_->window()->setWindowTitle(label + "'s screen");
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

ScreenDrawing* ScreenStage::drawing(const QString& name) const {
    StreamTile* tile = find(name);
    return tile ? tile->drawing() : nullptr;
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
    auto* window = new QWidget(this, Qt::Window);
    window->setAttribute(Qt::WA_DeleteOnClose);
    window->setWindowTitle(tile->displayName() + "'s screen");
    auto* layout = new QVBoxLayout(window);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(new DrawingControls(tile->drawing(), tile->own(), window));
    fullscreen_ = new StreamView(window);
    fullscreen_->setDrawing(tile->drawing());
    layout->addWidget(fullscreen_, 1);
    fullscreen_->setImage(tile->image());
    fullscreenName_ = name;
    connect(fullscreen_, &StreamView::doubleClicked, this, &ScreenStage::exitFullscreen);
    connect(fullscreen_, &StreamView::menuRequested, tile, &StreamTile::showVolumeMenu);
    connect(fullscreen_, &QObject::destroyed, this, [this, view = fullscreen_.data()] {
        if (fullscreen_.isNull() || fullscreen_ == view) {
            fullscreen_.clear();
            fullscreenName_.clear();
        }
        refreshEscapeShortcut();
    });
    refreshEscapeShortcut();
    window->showFullScreen();
    window->activateWindow();
}

void ScreenStage::exitFullscreen() {
    if (fullscreen_) {
        fullscreen_->window()->close();
        fullscreen_.clear();
    }
    fullscreenName_.clear();
    refreshEscapeShortcut();
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
