#include "desktop/desktop_notifier.h"

#include <QAction>
#include <QApplication>
#include <QMenu>
#include <QPixmap>
#include <QSystemTrayIcon>
#include <QWidget>

#if defined(Q_OS_MACOS)
#    include "desktop/mac_notifications.h"
#endif
#if defined(DISQUISITION_DBUS_NOTIFICATIONS)
#    include "desktop/freedesktop_notifications.h"
#endif
#if defined(Q_OS_WIN)
#    include "desktop/windows_notifications.h"
#endif

DesktopNotifier::DesktopNotifier(const QList<QAction*>& trayActions, QObject* parent) :
    QObject(parent), trayActions_(trayActions) {
#if defined(Q_OS_MACOS)
    mac_ = std::make_unique<MacNotifications>([this] { emit activated(); });
#else
    constexpr int kIconSizes[] = {16, 20, 24, 32, 40, 48, 64, 96, 128, 256};
    const QImage icon(QStringLiteral(":/disquisition.png"));
    const QImage smallIcon(QStringLiteral(":/disquisition-dark.png"));
    for (const int size : kIconSizes) {
        iconImages_.append((size <= 64 ? smallIcon : icon).scaled(size, size, Qt::KeepAspectRatio, Qt::SmoothTransformation));
        appIcon_.addPixmap(QPixmap::fromImage(iconImages_.last()));
    }
    updateIcons();
#    if defined(Q_OS_WIN)
    windows_ = new WindowsNotifications(smallIcon, this);
    if (windows_->isAvailable()) {
        connect(windows_, &WindowsNotifications::activated, this, &DesktopNotifier::activated);
    } else {
        delete windows_;
        windows_ = nullptr;
        showTrayIcon();
    }
#    elif defined(DISQUISITION_DBUS_NOTIFICATIONS)
    constexpr int kNotificationIconSize = 64;
    freedesktop_ = new FreedesktopNotifications(
        smallIcon.scaled(kNotificationIconSize, kNotificationIconSize, Qt::KeepAspectRatio,
                    Qt::SmoothTransformation),
        this);
    connect(freedesktop_, &FreedesktopNotifications::activated, this,
            &DesktopNotifier::activated);
    connect(freedesktop_, &FreedesktopNotifications::unavailable, this, [this] {
        freedesktop_->deleteLater();
        freedesktop_ = nullptr;
        showTrayIcon();
    });
#    else
    showTrayIcon();
#    endif
#endif
}

DesktopNotifier::~DesktopNotifier() {
    // The tray icon goes before the menu it shows.
    delete tray_;
}

void DesktopNotifier::show(const DesktopNotification& notification) {
#if defined(Q_OS_MACOS)
    mac_->show(notification.key, notification.title, notification.body);
#elif defined(Q_OS_WIN)
    if (windows_) {
        windows_->show(notification.key, notification.title, notification.body);
    } else {
        showTrayMessage(notification);
    }
#elif defined(DISQUISITION_DBUS_NOTIFICATIONS)
    if (freedesktop_) {
        freedesktop_->show(notification.key, notification.title, notification.body);
    } else {
        showTrayMessage(notification);
    }
#else
    showTrayMessage(notification);
#endif
}

void DesktopNotifier::withdrawAll() {
    // A tray icon's message cannot be taken back.
#if defined(Q_OS_MACOS)
    mac_->removeAll();
#elif defined(Q_OS_WIN)
    if (windows_) windows_->removeAll();
#elif defined(DISQUISITION_DBUS_NOTIFICATIONS)
    if (freedesktop_) freedesktop_->closeAll();
#endif
}

void DesktopNotifier::setBadge(const UnreadBadge& badge) {
    if (badge == badge_) return;
    badge_ = badge;
#if defined(Q_OS_MACOS)
    mac_->setBadgeLabel(badge_.label());
#else
    updateIcons();
#endif
}

void DesktopNotifier::requestAttention(QWidget* window) {
#if defined(Q_OS_MACOS)
    // Bouncing the Dock icon would repeat what its badge shows.
    Q_UNUSED(window);
#else
    QApplication::alert(window);
#endif
}

void DesktopNotifier::showTrayIcon() {
    if (tray_ || !QSystemTrayIcon::isSystemTrayAvailable()) return;
    trayMenu_ = std::make_unique<QMenu>();
    trayMenu_->addActions(trayActions_);
    tray_ = new QSystemTrayIcon(this);
    tray_->setContextMenu(trayMenu_.get());
    connect(tray_, &QSystemTrayIcon::messageClicked, this, &DesktopNotifier::activated);
    connect(tray_, &QSystemTrayIcon::activated, this,
            [this](QSystemTrayIcon::ActivationReason reason) {
        if (reason == QSystemTrayIcon::Trigger || reason == QSystemTrayIcon::DoubleClick) {
            emit activated();
        }
    });
    updateIcons();
    tray_->show();
}

void DesktopNotifier::showTrayMessage(const DesktopNotification& notification) {
    if (tray_ && QSystemTrayIcon::supportsMessages()) {
        tray_->showMessage(notification.title, notification.body, appIcon_);
    }
}

void DesktopNotifier::updateIcons() {
    QIcon icon;
    for (const QImage& image : std::as_const(iconImages_)) {
        icon.addPixmap(QPixmap::fromImage(drawBadge(image, badge_)));
    }
    QApplication::setWindowIcon(icon);
    if (tray_) {
        tray_->setIcon(icon);
        tray_->setToolTip(badgedTitle(QApplication::applicationDisplayName(), badge_));
    }
}
