#pragma once

#include <QIcon>
#include <QImage>
#include <QList>
#include <QObject>
#include <QString>
#include <memory>

#include "desktop/notification_rules.h"
#include "desktop/unread_badge.h"

class FreedesktopNotifications;
class MacNotifications;
class QAction;
class QMenu;
class QSystemTrayIcon;
class QWidget;

// Notification Center and the Dock on macOS, the D-Bus notification server on
// Linux, and a tray icon on Windows or when Linux has no notification server.
class DesktopNotifier final : public QObject {
    Q_OBJECT

public:
    explicit DesktopNotifier(const QList<QAction*>& trayActions, QObject* parent = nullptr);
    ~DesktopNotifier() override;

    void show(const DesktopNotification& notification);
    void withdrawAll();
    void setBadge(const UnreadBadge& badge);
    void requestAttention(QWidget* window);

signals:
    void activated();

private:
    void showTrayIcon();
    void showTrayMessage(const DesktopNotification& notification);
    void updateIcons();

    QList<QAction*> trayActions_;
    UnreadBadge badge_;
    QList<QImage> iconImages_;
    QIcon appIcon_;
    std::unique_ptr<QMenu> trayMenu_;
    QSystemTrayIcon* tray_ = nullptr;
    FreedesktopNotifications* freedesktop_ = nullptr;
#if defined(Q_OS_MACOS)
    std::unique_ptr<MacNotifications> mac_;
#endif
};
