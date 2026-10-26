#pragma once

#include <QElapsedTimer>
#include <QObject>
#include <QString>
#include <QTimer>

#include "desktop/mentions.h"
#include "desktop/notification_rules.h"
#include "desktop/unread_badge.h"

class DesktopNotifier;
class QAction;
class QEvent;
class QWidget;

class NotificationCenter final : public QObject {
    Q_OBJECT

public:
    explicit NotificationCenter(QWidget* window);
    ~NotificationCenter() override;

    const NotificationSettings& settings() const { return settings_; }
    void setSettings(const NotificationSettings& settings);
    QAction* muteAction() const { return muteAction_; }

    void messageReceived(const QString& sender, const QString& text, Mentioned mentioned);
    void activityOccurred(const QString& member, const QString& text);

signals:
    void settingsChanged(const NotificationSettings& settings);
    void badgeChanged(const UnreadBadge& badge);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    bool windowInView() const;
    void markReadIfInView();
    void setBadge(const UnreadBadge& badge);
    void notify(const QString& key, const QString& title, const QString& text);
    void show(const DesktopNotification& notification);
    void showDueNotifications();
    void schedulePacedNotifications();
    void bringWindowToFront();

    QWidget* window_;
    QAction* muteAction_ = nullptr;
    DesktopNotifier* notifier_ = nullptr;
    NotificationSettings settings_;
    UnreadBadge badge_;
    NotificationPacer pacer_;
    QElapsedTimer clock_;
    QTimer pacerTimer_;
    bool notified_ = false;
};
