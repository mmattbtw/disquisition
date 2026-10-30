#include "desktop/screen_stage.h"

#include <QContextMenuEvent>
#include <QLinearGradient>
#include <QMouseEvent>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QMenu>
#include <QPainter>
#include <QPushButton>
#include <QShortcut>
#include <QStackedWidget>
#include <QStyle>
#include <QVBoxLayout>

#include <cmath>

#include "desktop/volume_menu.h"

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

namespace {
QLabel* overlayLabel(const QString& text, const char* name, QWidget* parent) {
    auto* label = new QLabel(text, parent);
    label->setObjectName(name);
    label->setTextFormat(Qt::PlainText);
    label->setAttribute(Qt::WA_TransparentForMouseEvents);
    return label;
}
} // namespace

StreamTile::StreamTile(const QString& name, bool own, QWidget* parent, const QString& label)
    : QFrame(parent), name_(name), own_(own) {
    setObjectName("streamTile");
    setCursor(Qt::PointingHandCursor);

    // The header only shows while focused; in the grid the tile is just video.
    header_ = new QWidget(this);
    header_->setObjectName("streamHeader");
    auto* live = new QLabel("LIVE", header_);
    live->setObjectName("live");
    title_ = new QLabel(header_);
    title_->setObjectName("streamName");
    stats_ = new QLabel(header_);
    stats_->setObjectName("streamStats");
    // Lets the statistics give way first when the tile is narrow.
    stats_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    audience_ = new QPushButton(header_);
    audience_->setObjectName("streamAudience");
    audience_->setToolTip("See who is watching");
    audience_->hide();
    showAllButton_ = new QPushButton("Show All", header_);
    fullscreenButton_ = new QPushButton("Fullscreen", header_);
    stopButton_ = new QPushButton("Stop Watching", header_);
    auto* headerRow = new QHBoxLayout(header_);
    headerRow->setContentsMargins(12, 8, 8, 8);
    headerRow->setSpacing(7);
    headerRow->addWidget(live);
    headerRow->addWidget(title_);
    headerRow->addSpacing(4);
    headerRow->addWidget(stats_, 1);
    headerRow->addWidget(audience_);
    headerRow->addWidget(showAllButton_);
    headerRow->addWidget(fullscreenButton_);
    headerRow->addWidget(stopButton_);

    invitation_ = new QWidget(this);
    invitationText_ = new QLabel(invitation_);
    invitationText_->setObjectName("streamInvitation");
    invitationText_->setTextFormat(Qt::PlainText);
    invitationText_->setAlignment(Qt::AlignCenter);
    invitationText_->setWordWrap(true);
    auto* watchButton = new QPushButton("Watch Stream", invitation_);
    watchButton->setObjectName("primary");
    auto* invitationLayout = new QVBoxLayout(invitation_);
    invitationLayout->setSpacing(10);
    invitationLayout->addStretch();
    invitationLayout->addWidget(invitationText_);
    invitationLayout->addWidget(watchButton, 0, Qt::AlignHCenter);
    invitationLayout->addStretch();

    view_ = new StreamView(this);
    view_->setMinimumSize(16, 9);
    drawing_ = new ScreenDrawing(this);
    view_->setDrawing(drawing_);
    body_ = new QStackedWidget(this);
    body_->addWidget(invitation_);
    body_->addWidget(view_);

    // Floating over the video: the LIVE badge, the name and, when focused, the
    // drawing toolbar.
    quality_ = overlayLabel(QString(), "streamQuality", this);
    badge_ = overlayLabel("LIVE", "live", this);
    nameplate_ = overlayLabel(QString(), "streamNameplate", this);
    drawingControls_ = new DrawingControls(drawing_, own, this);
    drawingControls_->setFloating(true);
    drawingControls_->installEventFilter(this);
    view_->installEventFilter(this);
    setDisplayName(label.isEmpty() ? name : label);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(1, 1, 1, 1);
    layout->setSpacing(0);
    layout->addWidget(header_);
    layout->addWidget(body_, 1);

    connect(watchButton, &QPushButton::clicked, this, &StreamTile::watchClicked);
    connect(stopButton_, &QPushButton::clicked, this, &StreamTile::stopWatchingClicked);
    connect(showAllButton_, &QPushButton::clicked, this, &StreamTile::showAllClicked);
    connect(fullscreenButton_, &QPushButton::clicked, this, &StreamTile::fullscreenClicked);
    connect(audience_, &QPushButton::clicked, this, &StreamTile::showAudience);
    connect(view_, &StreamView::doubleClicked, this, &StreamTile::fullscreenClicked);
    connect(view_, &StreamView::menuRequested, this, &StreamTile::showVolumeMenu);
    connect(drawing_, &ScreenDrawing::changed, this, &StreamTile::refreshChrome);

    setWatching(own);
}

void StreamTile::setFocused(bool focused) {
    if (focused_ == focused)
        return;
    focused_ = focused;
    setCursor(focused ? Qt::ArrowCursor : Qt::PointingHandCursor);
    refreshChrome();
}

void StreamTile::refreshChrome() {
    header_->setVisible(focused_);
    badge_->setVisible(!focused_);
    quality_->setVisible(!focused_ && watching_ && !quality_->text().isEmpty());
    nameplate_->setVisible(!focused_);
    fullscreenButton_->setVisible(watching_);
    stopButton_->setVisible(watching_ && !own_);
    drawingControls_->setVisible(focused_ && watching_ && (own_ || drawing_->enabled()));
    placeOverlays();
}

void StreamTile::setAudience(const QList<QPixmap>& avatars, const QStringList& names) {
    audienceAvatars_ = avatars;
    audienceNames_ = names;
    constexpr int kSide = 20, kStep = 15, kShown = 5;
    const int shown = qMin<int>(avatars.size(), kShown);
    const qreal scale = devicePixelRatioF();
    const QSize size(shown ? (shown - 1) * kStep + kSide + 2 : 1, kSide + 2);
    QPixmap stack(size * scale);
    stack.setDevicePixelRatio(scale);
    stack.fill(Qt::transparent);
    QPainter painter(&stack);
    painter.setRenderHint(QPainter::Antialiasing);
    for (int i = 0; i < shown; ++i) {
        const QRectF tile(1 + i * kStep, 1, kSide, kSide);
        painter.setPen(QPen(QColor("#121418"), 2));
        painter.setBrush(Qt::NoBrush);
        painter.drawRoundedRect(tile.adjusted(-0.5, -0.5, 0.5, 0.5), 6, 6);
        painter.drawPixmap(tile.toRect(), avatars[i]);
    }
    painter.end();
    audience_->setIcon(QIcon(stack));
    audience_->setIconSize(size);
    audience_->setText(QString("%1 watching").arg(avatars.size()));
    audience_->setVisible(!avatars.isEmpty());
}

void StreamTile::showAudience() {
    QMenu menu(this);
    menu.setObjectName("streamAudienceList");
    auto* heading = menu.addAction(QString::fromUtf8("WATCHING — %1").arg(audienceNames_.size()));
    heading->setEnabled(false);
    menu.addSeparator();
    for (int i = 0; i < audienceNames_.size(); ++i)
        menu.addAction(QIcon(audienceAvatars_.value(i)), audienceNames_[i]);
    menu.exec(audience_->mapToGlobal(QPoint(0, audience_->height() + 4)));
}

void StreamTile::placeOverlays() {
    const QRect video = body_->geometry();
    const int inset = video.height() < 120 ? 6 : 10;
    badge_->adjustSize();
    badge_->move(video.right() - inset - badge_->width() + 1, video.top() + inset);
    quality_->adjustSize();
    quality_->move(badge_->x() - quality_->width() - 4, video.top() + inset);
    nameplate_->adjustSize();
    nameplate_->resize(qMin(nameplate_->width(), video.width() - inset * 2), nameplate_->height());
    nameplate_->move(video.left() + inset, video.bottom() - inset - nameplate_->height() + 1);
    for (auto* overlay : {static_cast<QWidget*>(quality_), static_cast<QWidget*>(badge_),
                          static_cast<QWidget*>(nameplate_)})
        overlay->raise();
    if (drawingControls_->isVisibleTo(this)) {
        const QSize size = drawingControls_->sizeHint();
        const int span = qMin(size.width(), width() - 16);
        drawingControls_->setGeometry(qMax(8, video.center().x() - span / 2), video.top() + 10,
                                      span, size.height());
        drawingControls_->raise();
    }
}

bool StreamTile::eventFilter(QObject* watched, QEvent* event) {
    // The toolbar's own layout changes, such as tools appearing, change its size.
    if ((watched == view_ && event->type() == QEvent::Resize) ||
        (watched == drawingControls_ && event->type() == QEvent::LayoutRequest))
        placeOverlays();
    // A click on the video focuses the tile, unless it is drawing.
    if (watched == view_ && event->type() == QEvent::MouseButtonRelease && !focused_ &&
        static_cast<QMouseEvent*>(event)->button() == Qt::LeftButton && !drawing_->penActive())
        emit focusClicked();
    return QFrame::eventFilter(watched, event);
}

void StreamTile::mouseReleaseEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton && !focused_)
        watching_ ? emit focusClicked() : emit watchClicked();
    QFrame::mouseReleaseEvent(event);
}

void StreamTile::resizeEvent(QResizeEvent* event) {
    QFrame::resizeEvent(event);
    placeOverlays();
}

void StreamTile::setDisplayName(const QString& label) {
    label_ = label;
    const QString shown = own_ ? label + " (you)" : label;
    title_->setText(QString("<b>%1</b>&nbsp; <span style='color:#8b959e'>is sharing</span>")
                        .arg(shown.toHtmlEscaped()));
    nameplate_->setText(shown);
    invitationText_->setText(label + " is sharing their screen");
    placeOverlays();
}

void StreamTile::setWatching(bool watching) {
    body_->setCurrentWidget(watching ? static_cast<QWidget*>(view_) : invitation_);
    watching_ = watching;
    if (!watching)
        drawing_->setState(false, 0);
    view_->clear();
    stats_->clear();
    quality_->clear();
    statsClock_.invalidate();
    statsFrames_ = 0;
    statsBytes_ = 0;
    refreshChrome();
}

void StreamTile::showVolumeMenu(const QPoint& globalPosition) {
    // Your own share's sound is never played back to you.
    if (own_ || !watching_)
        return;
    QMenu menu(this);
    addVolumeSlider(menu, volume_, [this](int percent) {
        volume_ = percent;
        emit volumeChanged(percent);
    });
    menu.exec(globalPosition);
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
    const double fps = statsFrames_ * 1000.0 / static_cast<double>(elapsed);
    QStringList parts;
    if (!detail_.isEmpty()) {
        parts << detail_;
    }
    parts << QString("%1x%2").arg(image.width()).arg(image.height())
          << QString("%1 fps").arg(fps, 0, 'f', 0)
          << QString("%1 Mbps").arg(static_cast<double>(statsBytes_) * 8.0 / 1000.0 /
                                        static_cast<double>(elapsed),
                                    0, 'f', 1);
    stats_->setText(parts.join(", "));
    quality_->setText(QString("%1P %2FPS").arg(image.height()).arg(fps, 0, 'f', 0));
    quality_->setVisible(!focused_ && watching_);
    placeOverlays();
    statsClock_.restart();
    statsFrames_ = 0;
    statsBytes_ = 0;
}

ParticipantTile::ParticipantTile(QWidget* parent) : QWidget(parent) {
    setObjectName("participantTile");
    setCursor(Qt::PointingHandCursor);
}

void ParticipantTile::setParticipant(const StageParticipant& person) {
    person_ = person;
    setAccessibleName(person.name);
    update();
}

void ParticipantTile::setSpeaking(bool speaking) {
    if (speaking_ == speaking)
        return;
    speaking_ = speaking;
    update();
}

void ParticipantTile::mouseReleaseEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton)
        emit clicked(event->globalPosition().toPoint());
}

void ParticipantTile::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setRenderHint(QPainter::SmoothPixmapTransform);
    // The glow needs room, so the tile itself sits a few pixels in.
    const QRectF tile = QRectF(rect()).adjusted(5, 5, -5, -5);
    const qreal radius = 12;
    if (speaking_) {
        for (int i = 5; i >= 1; --i) {
            painter.setPen(QPen(QColor(126, 217, 87, 80 - i * 13), 1));
            painter.setBrush(Qt::NoBrush);
            painter.drawRoundedRect(tile.adjusted(-i, -i, i, i), radius + i, radius + i);
        }
    }
    // Tint the tile with the picture's own color, darkened, as calls usually do.
    QColor tint("#2a2e36");
    if (!person_.avatar.isNull()) {
        const QColor average = person_.avatar.toImage().scaled(1, 1, Qt::IgnoreAspectRatio,
                                                               Qt::SmoothTransformation)
                                   .pixelColor(0, 0);
        if (average.alpha() > 0)
            tint = QColor::fromHsvF(average.hsvHueF() < 0 ? 0 : average.hsvHueF(),
                                    std::min(0.45, average.hsvSaturationF() * 0.6), 0.27);
    }
    QLinearGradient fill(tile.topLeft(), tile.bottomRight());
    fill.setColorAt(0, tint.lighter(118));
    fill.setColorAt(1, tint);
    painter.setBrush(fill);
    painter.setPen(QPen(speaking_ ? QColor("#7ed957") : QColor(255, 255, 255, 18),
                        speaking_ ? 2 : 1));
    painter.drawRoundedRect(tile.adjusted(0.5, 0.5, -0.5, -0.5), radius, radius);
    const qreal side = std::min<qreal>(104, tile.height() * 0.4);
    const QRectF avatar(tile.center().x() - side / 2, tile.center().y() - side / 2 - side * 0.08,
                        side, side);
    painter.drawPixmap(avatar.toRect(), person_.avatar);
    if (tile.height() < 50)
        return;
    QFont font = this->font();
    font.setPixelSize(tile.height() < 110 ? 11 : 13);
    font.setWeight(QFont::DemiBold);
    painter.setFont(font);
    const QFontMetricsF metrics(font);
    const qreal pad = tile.height() < 110 ? 6 : 10;
    QList<QPixmap> marks;
    const auto mark = [this](const QString& kind) {
        const qreal scale = devicePixelRatioF();
        QPixmap image(QSize(14, 14) * scale);
        image.setDevicePixelRatio(scale);
        image.fill(Qt::transparent);
        QPainter icon(&image);
        icon.setRenderHint(QPainter::Antialiasing);
        icon.setPen(QPen(kind == "sharing" ? QColor("#9dccca") : QColor("#ff8a8a"), 1.3,
                         Qt::SolidLine, Qt::RoundCap));
        if (kind == "sharing") {
            icon.drawRoundedRect(QRectF(1.5, 2, 11, 7.5), 1.5, 1.5);
            icon.drawLine(QPointF(7, 9.5), QPointF(7, 12));
            icon.drawLine(QPointF(4.5, 12.5), QPointF(9.5, 12.5));
        } else if (kind == "muted") {
            icon.drawRoundedRect(QRectF(5, 1.5, 4, 7), 2, 2);
            icon.drawArc(QRectF(3, 4, 8, 6.5), 200 * 16, 140 * 16);
            icon.drawLine(QPointF(7, 10.5), QPointF(7, 12.5));
            icon.drawLine(QPointF(2, 1.5), QPointF(12, 12.5));
        } else {
            icon.drawArc(QRectF(2.5, 2, 9, 10), 0, 180 * 16);
            icon.drawRoundedRect(QRectF(1.5, 7, 2.5, 5), 1, 1);
            icon.drawRoundedRect(QRectF(10, 7, 2.5, 5), 1, 1);
            icon.drawLine(QPointF(2, 1.5), QPointF(12, 12.5));
        }
        return image;
    };
    if (person_.sharing)
        marks.append(mark("sharing"));
    if (person_.muted)
        marks.append(mark("muted"));
    if (person_.deafened)
        marks.append(mark("deafened"));
    const qreal icons = marks.size() * 17;
    const auto name = metrics.elidedText(person_.name, Qt::ElideRight,
                                         tile.width() - pad * 2 - icons - 16);
    const QRectF label(tile.left() + pad, tile.bottom() - pad - metrics.height() - 6,
                       metrics.horizontalAdvance(name) + 14 + icons, metrics.height() + 6);
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(18, 20, 24, 190));
    painter.drawRoundedRect(label, 6, 6);
    qreal x = label.left() + 7;
    for (const auto& image : marks) {
        painter.drawPixmap(QPointF(x, label.center().y() - 7), image);
        x += 17;
    }
    painter.setPen(QColor("#edf0ef"));
    painter.drawText(QRectF(x, label.top(), label.right() - x - 7, label.height()),
                     Qt::AlignVCenter | Qt::AlignLeft, name);
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
    stripToggle_ = new QPushButton(this);
    stripToggle_->setObjectName("stageStripToggle");
    stripToggle_->setCursor(Qt::PointingHandCursor);
    connect(stripToggle_, &QPushButton::clicked, this, [this] {
        stripHidden_ = !stripHidden_;
        relayout();
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
    connect(tile, &StreamTile::focusClicked, this, [this, name] { setFocused(name); });
    connect(tile, &StreamTile::showAllClicked, this, [this] { setFocused({}); });
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
    tile->show();
    relayout();
    if (tiles_.size() == 1)
        emit opened();
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
        // Choosing to watch a share brings it to the front.
        if (watching)
            focused_ = name;
        if (!watching && focused_ == name)
            focused_.clear();
        relayout();
    }
    if (!watching && name == fullscreenName_) {
        exitFullscreen();
    }
}

void ScreenStage::setFocused(const QString& name) {
    focused_ = name;
    relayout();
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

void ScreenStage::setAudience(const QString& name, const QList<QPixmap>& avatars,
                              const QStringList& names) {
    if (StreamTile* tile = find(name))
        tile->setAudience(avatars, names);
}

void ScreenStage::setParticipants(const QList<StageParticipant>& people) {
    while (people_.size() > people.size())
        delete people_.takeLast();
    while (people_.size() < people.size()) {
        auto* tile = new ParticipantTile(this);
        connect(tile, &ParticipantTile::clicked, this, [this, tile](const QPoint& at) {
            emit participantClicked(tile->participant().id, at);
        });
        tile->show();
        people_.append(tile);
    }
    for (int i = 0; i < people.size(); ++i) {
        people_[i]->setParticipant(people[i]);
        people_[i]->setSpeaking(speaking_.contains(people[i].id));
    }
    relayout();
}

void ScreenStage::setSpeaking(const QString& id, bool speaking) {
    if (speaking)
        speaking_.insert(id);
    else
        speaking_.remove(id);
    for (auto* tile : std::as_const(people_))
        if (tile->participant().id == id)
            tile->setSpeaking(speaking);
}

QImage ScreenStage::image(const QString& name) const {
    StreamTile* tile = find(name);
    return tile ? tile->image() : QImage();
}

ScreenDrawing* ScreenStage::drawing(const QString& name) const {
    StreamTile* tile = find(name);
    return tile ? tile->drawing() : nullptr;
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
    auto* controls = new DrawingControls(tile->drawing(), tile->own(), window);
    controls->setFloating(true);
    layout->addWidget(controls, 0, Qt::AlignHCenter);
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

void ScreenStage::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    relayout();
}

namespace {
// The largest 16:9 tiles that fit `count` of them in `area`, centered, with
// a short last row centered too.
QList<QRect> gridTiles(const QRect& area, int count, int gap, int maxWidth) {
    QList<QRect> result;
    if (count == 0 || area.isEmpty())
        return result;
    int bestColumns = 1;
    double bestWidth = 0;
    for (int columns = 1; columns <= count; ++columns) {
        const int rows = (count + columns - 1) / columns;
        const double width =
            std::min((area.width() - (columns - 1) * gap) / double(columns),
                     (area.height() - (rows - 1) * gap) / double(rows) * 16.0 / 9.0);
        if (width > bestWidth + 0.5) {
            bestWidth = width;
            bestColumns = columns;
        }
    }
    const int width = std::max(0, int(std::min<double>(bestWidth, maxWidth)));
    const int height = width * 9 / 16;
    const int rows = (count + bestColumns - 1) / bestColumns;
    const int top = area.center().y() - (rows * height + (rows - 1) * gap) / 2;
    for (int i = 0; i < count; ++i) {
        const int row = i / bestColumns;
        const int inRow = std::min(bestColumns, count - row * bestColumns);
        const int left = area.center().x() - (inRow * width + (inRow - 1) * gap) / 2;
        result.append(QRect(left + (i % bestColumns) * (width + gap),
                            top + row * (height + gap), width, height));
    }
    return result;
}

// People tiles keep a margin for their talking glow; shares match their body.
QRect fitted(QWidget* tile, const QRect& cell) {
    return qobject_cast<StreamTile*>(tile) ? cell.adjusted(5, 5, -5, -5) : cell;
}
} // namespace

void ScreenStage::relayout() {
    StreamTile* focused = find(focused_);
    if (!focused)
        focused_.clear();
    QList<QWidget*> others;
    for (StreamTile* tile : std::as_const(tiles_)) {
        tile->setFocused(tile == focused);
        if (tile != focused)
            others.append(tile);
    }
    for (ParticipantTile* tile : std::as_const(people_))
        others.append(tile);
    const QRect area = rect();
    constexpr int kGap = 10;
    if (!focused) {
        stripToggle_->hide();
        const auto tiles = gridTiles(area, int(others.size()), kGap, 720);
        for (int i = 0; i < others.size(); ++i) {
            others[i]->setGeometry(fitted(others[i], tiles[i]));
            others[i]->show();
        }
        return;
    }
    // The focused share fills the stage; everything else waits in a strip
    // beneath it, which can be tucked away.
    constexpr int kStrip = 96, kToggle = 20;
    const bool strip = !others.isEmpty();
    const bool showStrip = strip && !stripHidden_;
    const int reserved = strip ? kToggle + 8 + (showStrip ? kStrip + 6 : 0) : 0;
    focused->setGeometry(area.left(), area.top(), area.width(), area.height() - reserved);
    focused->show();
    stripToggle_->setVisible(strip);
    if (strip) {
        stripToggle_->setIcon(QIcon(stripHidden_ ? ":/ui/chevron-up.png" : ":/ui/chevron-down.png"));
        stripToggle_->setText(stripHidden_ ? "Show call" : "Hide call");
        stripToggle_->setToolTip(stripHidden_ ? "Show everyone in the call"
                                              : "Hide everyone else in the call");
        stripToggle_->adjustSize();
        stripToggle_->setGeometry(area.center().x() - stripToggle_->width() / 2,
                                  focused->geometry().bottom() + 6, stripToggle_->width(), kToggle);
        stripToggle_->raise();
    }
    const QRect row(area.left(), area.bottom() - kStrip + 1, area.width(), kStrip);
    const auto tiles = gridTiles(row, int(others.size()), 8, kStrip * 16 / 9);
    for (int i = 0; i < others.size(); ++i) {
        others[i]->setGeometry(fitted(others[i], tiles.value(i)));
        others[i]->setVisible(showStrip);
    }
}
