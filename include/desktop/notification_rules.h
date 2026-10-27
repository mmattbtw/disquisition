#pragma once

#include <QHash>
#include <QList>
#include <QString>
#include <optional>

#include "desktop/mentions.h"

class QSettings;

struct DesktopNotification {
    // Notifications with the same key replace one another.
    QString key;
    QString title;
    QString body;
};

enum class NotifyLevel { AllMessages, MentionsOnly };

struct NotificationSettings {
    // Notifications, unread markers, and app-icon badges are opt-in.
    bool muted = true;
    NotifyLevel level = NotifyLevel::AllMessages;
    bool suppressEveryone = false;
    // Joins and screen shares.
    bool activity = true;

    static NotificationSettings load(const QSettings& settings);
    void save(QSettings& settings) const;

    bool operator==(const NotificationSettings&) const = default;
};

struct MessageAttention {
    bool mention = false; // counts toward the badge number
    bool notify = false;
    bool alert = false;   // flashes the taskbar entry
};

MessageAttention attentionFor(const NotificationSettings& settings, Mentioned mentioned);
bool notifiesActivity(const NotificationSettings& settings);

// Windows cuts notification text at 255 characters.
constexpr qsizetype kMaxNotificationText = 200;

QString notificationText(const QString& text);

constexpr qint64 kNotificationCooldownMs = 5000;

// The first notification for a key shows at once, then at most one per
// cooldown, with the latest text.
class NotificationPacer {
public:
    explicit NotificationPacer(qint64 cooldownMs = kNotificationCooldownMs);

    bool offer(const DesktopNotification& notification, qint64 now);
    QList<DesktopNotification> takeDue(qint64 now);
    std::optional<qint64> nextDue() const;
    void clear();

private:
    struct Waiting {
        DesktopNotification notification;
        qint64 due = 0;
    };

    qint64 cooldownMs_;
    QHash<QString, qint64> lastShown_;
    QHash<QString, Waiting> waiting_;
};
