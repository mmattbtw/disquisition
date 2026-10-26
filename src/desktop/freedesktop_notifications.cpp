#include "desktop/freedesktop_notifications.h"

#include <QByteArray>
#include <QDBusArgument>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusMetaType>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QGuiApplication>
#include <QStringList>
#include <QVariantMap>
#include <algorithm>

namespace {

const QString kService = QStringLiteral("org.freedesktop.Notifications");
const QString kPath = QStringLiteral("/org/freedesktop/Notifications");
const QString kInterface = QStringLiteral("org.freedesktop.Notifications");
// Clicking the notification itself invokes the action with this key.
const QString kDefaultAction = QStringLiteral("default");

// The specification's "image-data" hint: (iiibiiay) for width, height, row
// stride, alpha, bits per sample, channels and RGBA bytes.
struct NotificationImage {
    QImage image; // Format_RGBA8888
};

QDBusArgument& operator<<(QDBusArgument& argument, const NotificationImage& value) {
    const QImage& image = value.image;
    argument.beginStructure();
    argument << image.width() << image.height() << static_cast<int>(image.bytesPerLine()) << true
             << 8 << 4
             << QByteArray(reinterpret_cast<const char*>(image.constBits()), image.sizeInBytes());
    argument.endStructure();
    return argument;
}

// QtDBus needs both directions, though images only ever go to the server.
const QDBusArgument& operator>>(const QDBusArgument& argument, NotificationImage& value) {
    int width = 0;
    int height = 0;
    int stride = 0;
    bool alpha = false;
    int bits = 0;
    int channels = 0;
    QByteArray bytes;
    argument.beginStructure();
    argument >> width >> height >> stride >> alpha >> bits >> channels >> bytes;
    argument.endStructure();
    value.image = {};
    if (width > 0 && height > 0 && alpha && bits == 8 && channels == 4 && stride >= width * 4 &&
        bytes.size() >= static_cast<qsizetype>(stride) * height) {
        value.image = QImage(reinterpret_cast<const uchar*>(bytes.constData()), width, height,
                             stride, QImage::Format_RGBA8888)
                          .copy();
    }
    return argument;
}

}  // namespace

Q_DECLARE_METATYPE(NotificationImage)

FreedesktopNotifications::FreedesktopNotifications(const QImage& icon, QObject* parent) :
    QObject(parent), icon_(icon.convertToFormat(QImage::Format_RGBA8888)) {
    qDBusRegisterMetaType<NotificationImage>();
    QDBusConnection bus = QDBusConnection::sessionBus();
    bus.connect(kService, kPath, kInterface, "ActionInvoked", this,
                SLOT(actionInvoked(uint,QString)));
    bus.connect(kService, kPath, kInterface, "ActivationToken", this,
                SLOT(activationToken(uint,QString)));
    bus.connect(kService, kPath, kInterface, "NotificationClosed", this,
                SLOT(notificationClosed(uint,uint)));
    probe();
}

void FreedesktopNotifications::show(const QString& key, const QString& title,
                                    const QString& body) {
    QVariantMap hints;
    hints.insert("category", QStringLiteral("im.received"));
    hints.insert("urgency", QVariant::fromValue<uchar>(1)); // normal
    if (!icon_.isNull()) hints.insert("image-data", QVariant::fromValue(NotificationImage{icon_}));
    QDBusMessage message = QDBusMessage::createMethodCall(kService, kPath, kInterface, "Notify");
    // Only the body may be markup.
    message << QGuiApplication::applicationDisplayName() << showing_.value(key) << QString()
            << title << (bodyMarkup_ ? body.toHtmlEscaped() : body)
            << (actions_ ? QStringList{kDefaultAction, QStringLiteral("Open")} : QStringList())
            << hints << -1; // the server's default timeout
    auto* call = new QDBusPendingCallWatcher(QDBusConnection::sessionBus().asyncCall(message), this);
    connect(call, &QDBusPendingCallWatcher::finished, this,
            [this, key](QDBusPendingCallWatcher* finished) {
        finished->deleteLater();
        const QDBusPendingReply<uint> reply = *finished;
        if (!reply.isError()) showing_.insert(key, reply.value());
    });
}

void FreedesktopNotifications::closeAll() {
    QDBusConnection bus = QDBusConnection::sessionBus();
    for (const uint id : std::as_const(showing_)) {
        QDBusMessage message =
            QDBusMessage::createMethodCall(kService, kPath, kInterface, "CloseNotification");
        message << id;
        bus.send(message);
    }
    showing_.clear();
}

void FreedesktopNotifications::probe() {
    QDBusConnection bus = QDBusConnection::sessionBus();
    if (!bus.isConnected()) {
        QMetaObject::invokeMethod(this, [this] { emit unavailable(); }, Qt::QueuedConnection);
        return;
    }
    // This also starts a server that D-Bus launches on demand.
    auto* call = new QDBusPendingCallWatcher(
        bus.asyncCall(
            QDBusMessage::createMethodCall(kService, kPath, kInterface, "GetCapabilities")),
        this);
    connect(call, &QDBusPendingCallWatcher::finished, this,
            [this](QDBusPendingCallWatcher* finished) {
        finished->deleteLater();
        const QDBusPendingReply<QStringList> reply = *finished;
        if (reply.isError()) {
            emit unavailable();
            return;
        }
        bodyMarkup_ = reply.value().contains("body-markup");
        actions_ = reply.value().contains("actions");
    });
}

bool FreedesktopNotifications::owns(uint id) const {
    return std::find(showing_.cbegin(), showing_.cend(), id) != showing_.cend();
}

// Servers may broadcast these signals to every client, so each checks the id.

void FreedesktopNotifications::actionInvoked(uint id, const QString& action) {
    if (action == kDefaultAction && owns(id)) emit activated();
}

// Sent just before ActionInvoked. On Wayland, Qt raises the window with it.
void FreedesktopNotifications::activationToken(uint id, const QString& token) {
    if (owns(id) && !token.isEmpty()) qputenv("XDG_ACTIVATION_TOKEN", token.toUtf8());
}

void FreedesktopNotifications::notificationClosed(uint id, uint) {
    showing_.removeIf([id](const auto& entry) { return entry.value() == id; });
}
