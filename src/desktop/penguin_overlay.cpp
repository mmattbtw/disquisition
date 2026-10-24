#include "desktop/penguin_overlay.h"
#include "desktop/chat_color.h"

#include <QApplication>
#include <QDateTime>
#include <QElapsedTimer>
#include <QFontMetrics>
#include <QImage>
#include <QPainter>
#include <QPainterPath>
#include <QRandomGenerator>
#include <QScreen>
#include <QTimer>
#include <QWidget>
#include <QWindow>
#include <algorithm>
#include <array>
#include <cmath>

#if defined(Q_OS_WIN)
#    include <qt_windows.h>
#endif

// Referencing the resource initializer also pulls the compiled PNGs out of the
// static library when linking the desktop executable.
static void initializePenguins() {
    Q_INIT_RESOURCE(penguins);
}

#if defined(Q_OS_MACOS)
void configureMacPenguinOverlay(QWidget* widget);
bool macPenguinReducedMotion();
#endif

namespace {

constexpr int kSpriteHeight = 76;
constexpr int kOverlayHeight = 300;
constexpr double kEdge = 40;

quint32 nameHash(const QString& name) {
    // Give each name a consistent starting position and lane.
    quint32 hash = 2166136261u;
    for (unsigned char byte : name.toUtf8()) hash = (hash ^ byte) * 16777619u;
    return hash;
}

bool reducedMotion() {
#if defined(Q_OS_MACOS)
    return macPenguinReducedMotion();
#elif defined(Q_OS_WIN)
    BOOL animations = TRUE;
    SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &animations, 0);
    return !animations;
#else
    return false;
#endif
}

QImage tintedFrame(const QImage& source, QColor color) {
    QImage image = source.convertToFormat(QImage::Format_ARGB32);
    for (int y = 0; y < image.height(); ++y) {
        auto* pixels = reinterpret_cast<QRgb*>(image.scanLine(y));
        for (int x = 0; x < image.width(); ++x) {
            const QRgb pixel = pixels[x];
            // The supplied side-walk frames are already assembled. Replace only
            // blue plumage, retaining the beak, belly, feet, alpha and shading.
            if (qBlue(pixel) > qRed(pixel) * 1.2 && qBlue(pixel) > qGreen(pixel) * 1.2) {
                const double shade = qBlue(pixel) / 168.0;
                pixels[x] = qRgba(qMin(255, qRound(color.red() * shade)),
                                  qMin(255, qRound(color.green() * shade)),
                                  qMin(255, qRound(color.blue() * shade)), qAlpha(pixel));
            }
        }
    }
    // Cache at 2x the displayed size, keeping Retina edges sharp without a
    // full-resolution copy of every animation frame for every participant.
    const double scale = 2.0 * kSpriteHeight / 442.0;
    return image.scaled(qRound(image.width() * scale), qRound(image.height() * scale),
                        Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
}

}  // namespace

void PenguinActivity::setMembers(const QStringList& names, qint64 nowMs) {
    for (auto it = lastActivity_.begin(); it != lastActivity_.end();) {
        if (!names.contains(it.key())) it = lastActivity_.erase(it);
        else ++it;
    }
    for (const QString& name : names) {
        if (!name.isEmpty() && !lastActivity_.contains(name)) lastActivity_.insert(name, nowMs);
    }
}

void PenguinActivity::noteActivity(const QString& name, qint64 nowMs) {
    auto member = lastActivity_.find(name);
    if (member != lastActivity_.end()) member.value() = nowMs;
}

QStringList PenguinActivity::activeMembers(qint64 nowMs, int timeoutMinutes) const {
    QStringList names;
    const qint64 timeoutMs = static_cast<qint64>(timeoutMinutes) * 60 * 1000;
    for (auto it = lastActivity_.cbegin(); it != lastActivity_.cend(); ++it) {
        if (timeoutMinutes <= 0 || nowMs - it.value() < timeoutMs) names.append(it.key());
    }
    return names;
}

void PenguinScene::setMembers(QStringList names) {
    names.removeAll(QString());
    names.removeDuplicates();
    for (auto it = penguins_.begin(); it != penguins_.end();) {
        if (!names.contains(it.key())) it = penguins_.erase(it);
        else ++it;
    }
    for (const QString& name : names) {
        if (penguins_.contains(name)) continue;
        const quint32 hash = nameHash(name);
        DesktopPenguin penguin;
        penguin.color = chatColor("mint");
        const double edge = qMin(kEdge, size_.width() / 2);
        penguin.x = edge + (size_.width() - 2 * edge) * ((hash % 1000) / 999.0);
        penguin.destination = penguin.x;
        penguin.idleSeconds = 0.3 + (hash % 2000) / 1000.0;
        penguin.lane = static_cast<int>((hash / 1000) % 3);
        penguin.facingRight = (hash & 1) != 0;
        penguins_.insert(name, penguin);
    }
}

void PenguinScene::setColor(const QString& name, const QColor& color) {
    auto penguin = penguins_.find(name);
    if (penguin != penguins_.end() && color.isValid()) penguin->color = color;
}

void PenguinScene::setSize(QSizeF size) {
    const double oldWidth = size_.width();
    size_ = size.expandedTo(QSizeF(1, 1));
    const double edge = qMin(kEdge, size_.width() / 2);
    for (auto& penguin : penguins_) {
        penguin.x = qBound(edge, penguin.x * size_.width() / oldWidth, size_.width() - edge);
        penguin.destination = qBound(edge, penguin.destination * size_.width() / oldWidth,
                                     size_.width() - edge);
    }
}

void PenguinScene::advance(double seconds, bool wandering) {
    if (!std::isfinite(seconds) || seconds <= 0) return;
    // A sleep/wake or a busy event loop must not fling characters across the
    // display. Speech still expires against the full elapsed time.
    const double step = qMin(seconds, 0.1);
    for (auto& penguin : penguins_) {
        penguin.speechSeconds = qMax(0.0, penguin.speechSeconds - seconds);
        if (penguin.speechSeconds == 0) penguin.speech.clear();
        if (!wandering || !penguin.speech.isEmpty()) continue;
        if (penguin.idleSeconds > 0) {
            penguin.idleSeconds -= step;
            if (penguin.idleSeconds <= 0) {
                const double edge = qMin(kEdge, size_.width() / 2);
                penguin.destination = edge + QRandomGenerator::global()->generateDouble() *
                                                (size_.width() - 2 * edge);
                penguin.facingRight = penguin.destination > penguin.x;
                penguin.walkSeconds = 0;
            }
            continue;
        }
        const double distance = penguin.destination - penguin.x;
        const double stride = 28 * step;
        if (std::abs(distance) <= stride) {
            penguin.x = penguin.destination;
            penguin.idleSeconds = 1.0 + QRandomGenerator::global()->generateDouble() * 3;
        } else {
            penguin.x += std::copysign(stride, distance);
            penguin.walkSeconds += step;
        }
    }
}

void PenguinScene::showMessage(const QString& sender, const QString& body) {
    auto penguin = penguins_.find(sender);
    if (penguin == penguins_.end()) return;
    QString text = body.simplified();
    if (text.isEmpty()) return;
    if (text.size() > 180) {
        text.truncate(180);
        if (text.back().isHighSurrogate()) text.chop(1);
        text += QChar(0x2026);
    }
    penguin->speech = text;
    penguin->speechSeconds = qBound(5.0, 3.0 + text.size() * 0.055, 12.0);
}

class PenguinOverlayWindow final : public QWidget {
public:
    explicit PenguinOverlayWindow(QScreen* screen)
        : QWidget(nullptr, Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint |
                               Qt::WindowTransparentForInput | Qt::WindowDoesNotAcceptFocus |
                               Qt::NoDropShadowWindowHint) {
        setObjectName("penguinOverlay");
        setWindowTitle("disquisition penguins");
        setAttribute(Qt::WA_TranslucentBackground);
        setAttribute(Qt::WA_TransparentForMouseEvents);
        setAttribute(Qt::WA_ShowWithoutActivating);
        setAttribute(Qt::WA_QuitOnClose, false);
        setFocusPolicy(Qt::NoFocus);
        // Give Qt the target screen before creating the native window.
        setScreen(screen);
        auto place = [this, screen] {
            const QRect available = screen->availableGeometry();
            const int height = qMin(kOverlayHeight, available.height());
            setGeometry(available.x(), available.y() + available.height() - height,
                        available.width(), height);
            scene_.setSize(size());
        };
        place();
        connect(screen, &QScreen::availableGeometryChanged, this, place);
        for (int i = 0; i < 8; ++i) {
            frames_[i] = QImage(QString(":/penguins/walk/%1.png").arg(i + 1));
        }
        systemReducedMotion_ = reducedMotion();
        clock_.start();
        timer_.setInterval(33);
        connect(&timer_, &QTimer::timeout, this, [this] {
            const double seconds = clock_.restart() / 1000.0;
            motionCheckSeconds_ += seconds;
            if (motionCheckSeconds_ >= 1) {
                systemReducedMotion_ = reducedMotion();
                motionCheckSeconds_ = 0;
            }
            const bool moving = wandering_ && !systemReducedMotion_;
            scene_.advance(seconds, moving);
            bool speaking = false;
            for (const auto& penguin : scene_.penguins()) speaking |= !penguin.speech.isEmpty();
            timer_.setInterval(moving ? 33 : speaking ? 200 : 1000);
            if (moving || speaking || hadSpeech_) update();
            hadSpeech_ = speaking;
        });
    }

    void setMembers(const QStringList& names) {
        scene_.setMembers(names);
        for (auto it = sprites_.begin(); it != sprites_.end();) {
            if (!names.contains(it.key())) it = sprites_.erase(it);
            else ++it;
        }
        if (!isVisible()) {
            show();
#if defined(Q_OS_MACOS)
            if (QGuiApplication::platformName() == "cocoa") configureMacPenguinOverlay(this);
#endif
            clock_.restart();
            timer_.start();
        }
        update();
    }

    void setWandering(bool wandering) {
        wandering_ = wandering;
        timer_.setInterval(wandering ? 33 : 200);
        update();
    }
    void setCustomSprites(const QString& name, const QImage& standing, const QImage& walking) {
        if (standing.isNull() || walking.isNull()) sprites_.remove(name);
        else sprites_.insert(name, qMakePair(standing, walking));
        update();
    }
    void clearCustomSprites() {
        sprites_.clear();
        update();
    }
    void setColor(const QString& name, const QColor& color) {
        const auto penguin = scene_.penguins().constFind(name);
        if (penguin == scene_.penguins().cend() || penguin->color == color) return;
        scene_.setColor(name, color);
        // Color changes must not accumulate obsolete animation frames.
        tinted_.clear();
        update();
    }
    void showMessage(const QString& sender, const QString& body) {
        scene_.showMessage(sender, body);
        timer_.setInterval(33);
        update();
    }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setRenderHint(QPainter::SmoothPixmapTransform);
        QFont font = QApplication::font();
        font.setPixelSize(12);
        font.setWeight(QFont::DemiBold);
        painter.setFont(font);
        const QFontMetrics metrics(font);
        QList<QRectF> bubbles;
        // Draw back lanes first so feet overlap naturally.
        for (int lane = 2; lane >= 0; --lane) {
            for (auto it = scene_.penguins().cbegin(); it != scene_.penguins().cend(); ++it) {
                const DesktopPenguin& penguin = it.value();
                if (penguin.lane != lane) continue;
                const double feet = height() - 27 - lane * 10;
                const bool walking = wandering_ && !systemReducedMotion_ &&
                                     penguin.idleSeconds <= 0 && penguin.speech.isEmpty();
                const int frame = walking ? static_cast<int>(penguin.walkSeconds * 10) % 8 : 2;
                const auto sprites = sprites_.constFind(it.key());
                const bool custom = sprites != sprites_.cend();
                QImage image;
                QSizeF spriteSize;
                if (custom) {
                    image = walking ? sprites->second : sprites->first;
                    spriteSize = QSizeF(image.size()).scaled(kSpriteHeight, kSpriteHeight,
                                                            Qt::KeepAspectRatio);
                } else {
                    const auto key = qMakePair(penguin.color.rgba(), frame);
                    if (!tinted_.contains(key)) {
                        tinted_.insert(key, tintedFrame(frames_[frame], penguin.color));
                    }
                    image = tinted_[key];
                    const double scale = kSpriteHeight / 442.0;
                    spriteSize = QSizeF(frames_[frame].size()) * scale;
                }
                painter.save();
                painter.translate(penguin.x, feet);
                // Keep the forward-facing custom standing image unmirrored.
                if (penguin.facingRight && (!custom || walking)) painter.scale(-1, 1);
                painter.drawImage(QRectF(-spriteSize.width() / 2, -spriteSize.height(),
                                        spriteSize.width(), spriteSize.height()), image);
                painter.restore();

                const QString name = metrics.elidedText(it.key(), Qt::ElideRight, 130);
                const double labelWidth = metrics.horizontalAdvance(name) + 12;
                const double labelX = qBound(2.0, penguin.x - labelWidth / 2,
                                             qMax(2.0, width() - labelWidth - 2));
                const QRectF label(labelX, feet + 2, labelWidth, 20);
                painter.setPen(Qt::NoPen);
                painter.setBrush(QColor(19, 29, 42, 215));
                painter.drawRoundedRect(label, 7, 7);
                painter.setPen(QColor("#f5f8fc"));
                painter.drawText(label, Qt::AlignCenter, name);

                if (penguin.speech.isEmpty()) continue;
                const int textWidth = qMax(20, qMin(210, width() - 36));
                const int flags = Qt::TextWordWrap | Qt::TextWrapAnywhere | Qt::AlignLeft;
                const QRect textBounds = metrics.boundingRect(QRect(0, 0, textWidth, 200),
                                                               flags, penguin.speech);
                const double bubbleWidth = qMin(textWidth, textBounds.width()) + 20;
                const double bubbleHeight = textBounds.height() + 16;
                const double x = qBound(4.0, penguin.x - bubbleWidth / 2,
                                         qMax(4.0, width() - bubbleWidth - 4));
                const double head = feet - kSpriteHeight;
                QRectF bubble(x, head - bubbleHeight - 10, bubbleWidth, bubbleHeight);
                // Separate concurrent nearby messages where the display has room.
                for (const QRectF& other : bubbles) {
                    if (bubble.intersects(other.adjusted(-3, -3, 3, 3))) {
                        bubble.moveBottom(other.top() - 6);
                    }
                }
                bubble.moveTop(qMax(4.0, bubble.top()));
                bubbles.append(bubble);
                painter.setPen(QPen(QColor("#25354b"), 1.5));
                painter.setBrush(QColor("#fffdf7"));
                const double tailX = qBound(bubble.left() + 10, penguin.x, bubble.right() - 10);
                QPainterPath path;
                path.addRoundedRect(bubble, 12, 12);
                QPainterPath tail;
                tail.moveTo(tailX - 5, bubble.bottom() - 1);
                tail.lineTo(penguin.x, qMin(head - 2, bubble.bottom() + 10));
                tail.lineTo(tailX + 5, bubble.bottom() - 1);
                tail.closeSubpath();
                painter.drawPath(path.united(tail));
                painter.drawText(bubble.adjusted(10, 8, -10, -8), flags, penguin.speech);
            }
        }
    }

private:
    PenguinScene scene_;
    std::array<QImage, 8> frames_;
    QMap<QString, QPair<QImage, QImage>> sprites_;
    QMap<QPair<QRgb, int>, QImage> tinted_;
    QTimer timer_;
    QElapsedTimer clock_;
    double motionCheckSeconds_ = 0;
    bool wandering_ = true;
    bool systemReducedMotion_ = false;
    bool hadSpeech_ = false;
};

PenguinOverlay::PenguinOverlay(QObject* parent, std::function<qint64()> now)
    : QObject(parent), now_(now ? std::move(now) : QDateTime::currentMSecsSinceEpoch) {
    initializePenguins();
    activityTimer_.setInterval(1000);
    connect(&activityTimer_, &QTimer::timeout, this, [this] {
        if (activity_.activeMembers(now_(), idleTimeoutMinutes_) != visibleMembers_) syncScreens();
    });
    activityTimer_.start();
    connect(qApp, &QGuiApplication::screenAdded, this, [this] { syncScreens(); });
    connect(qApp, &QGuiApplication::screenRemoved, this, [this](QScreen* screen) {
        windows_.erase(screen);
        syncScreens();
    });
}

PenguinOverlay::~PenguinOverlay() = default;

void PenguinOverlay::setMembers(QStringList names) {
    names.removeAll(QString());
    names.removeDuplicates();
    names.sort();
    if (members_ == names) return;
    members_ = std::move(names);
    for (auto it = sprites_.begin(); it != sprites_.end();) {
        if (!members_.contains(it.key())) it = sprites_.erase(it);
        else ++it;
    }
    activity_.setMembers(members_, now_());
    for (auto it = colors_.begin(); it != colors_.end();) {
        if (!members_.contains(it.key())) it = colors_.erase(it);
        else ++it;
    }
    syncScreens();
}

void PenguinOverlay::setColor(const QString& name, const QColor& color) {
    if (!color.isValid() || !members_.contains(name) || colors_.value(name) == color) return;
    colors_.insert(name, color);
    for (auto& [screen, window] : windows_) window->setColor(name, color);
}

void PenguinOverlay::setEnabled(bool enabled) {
    if (enabled_ == enabled) return;
    enabled_ = enabled;
    syncScreens();
}

void PenguinOverlay::setWandering(bool wandering) {
    wandering_ = wandering;
    for (auto& [screen, window] : windows_) window->setWandering(wandering);
}

void PenguinOverlay::setCustomSprites(const QString& name, const QImage& standing,
                                      const QImage& walking) {
    if (!members_.contains(name)) return;
    if (standing.isNull() || walking.isNull()) sprites_.remove(name);
    else sprites_.insert(name, qMakePair(standing, walking));
    for (auto& [screen, window] : windows_) {
        window->setCustomSprites(name, standing, walking);
    }
}

void PenguinOverlay::clearCustomSprites() {
    sprites_.clear();
    for (auto& [screen, window] : windows_) window->clearCustomSprites();
}

void PenguinOverlay::setIdleTimeoutMinutes(int minutes) {
    idleTimeoutMinutes_ = qBound(0, minutes, 1440);
    syncScreens();
}

void PenguinOverlay::noteActivity(const QString& name) {
    activity_.noteActivity(name, now_());
    if (activity_.activeMembers(now_(), idleTimeoutMinutes_) != visibleMembers_) syncScreens();
}

void PenguinOverlay::showMessage(const QString& sender, const QString& body) {
    if (body.trimmed().isEmpty()) return;
    noteActivity(sender);
    for (auto& [screen, window] : windows_) window->showMessage(sender, body);
}

void PenguinOverlay::syncScreens() {
    visibleMembers_ = activity_.activeMembers(now_(), idleTimeoutMinutes_);
    if (!enabled_ || visibleMembers_.isEmpty()) {
        windows_.clear();
        return;
    }
    const auto screens = QGuiApplication::screens();
    for (auto it = windows_.begin(); it != windows_.end();) {
        if (!screens.contains(it->first)) it = windows_.erase(it);
        else ++it;
    }
    for (QScreen* screen : screens) {
        auto& window = windows_[screen];
        if (!window) window = std::make_unique<PenguinOverlayWindow>(screen);
        window->setWandering(wandering_);
        window->setMembers(visibleMembers_);
        for (auto it = sprites_.cbegin(); it != sprites_.cend(); ++it) {
            if (visibleMembers_.contains(it.key())) {
                window->setCustomSprites(it.key(), it->first, it->second);
            }
        }
        for (auto it = colors_.cbegin(); it != colors_.cend(); ++it) {
            window->setColor(it.key(), it.value());
        }
    }
}
