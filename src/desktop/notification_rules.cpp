#include "desktop/notification_rules.h"

#include <QSettings>

namespace {

const QString kMentionsOnly = QStringLiteral("mentions");
const QString kAllMessages = QStringLiteral("all");

}  // namespace

NotificationSettings NotificationSettings::load(const QSettings& settings) {
    const NotificationSettings defaults;
    NotificationSettings loaded;
    loaded.muted = settings.value("notifications/muted", defaults.muted).toBool();
    loaded.level = settings.value("notifications/level").toString() == kMentionsOnly
                       ? NotifyLevel::MentionsOnly
                       : NotifyLevel::AllMessages;
    loaded.suppressEveryone =
        settings.value("notifications/suppressEveryone", defaults.suppressEveryone).toBool();
    loaded.activity = settings.value("notifications/activity", defaults.activity).toBool();
    return loaded;
}

void NotificationSettings::save(QSettings& settings) const {
    settings.setValue("notifications/muted", muted);
    settings.setValue("notifications/level",
                      level == NotifyLevel::MentionsOnly ? kMentionsOnly : kAllMessages);
    settings.setValue("notifications/suppressEveryone", suppressEveryone);
    settings.setValue("notifications/activity", activity);
}

MessageAttention attentionFor(const NotificationSettings& settings, Mentioned mentioned) {
    MessageAttention attention;
    attention.mention = mentioned == Mentioned::ByName ||
                        (mentioned == Mentioned::ViaEveryone && !settings.suppressEveryone);
    attention.notify =
        !settings.muted && (settings.level == NotifyLevel::AllMessages || attention.mention);
    attention.alert = !settings.muted && attention.mention;
    return attention;
}

bool notifiesActivity(const NotificationSettings& settings) {
    return !settings.muted && settings.activity;
}

QString notificationText(const QString& text) {
    const QString line = text.simplified();
    if (line.size() <= kMaxNotificationText) return line;
    qsizetype cut = kMaxNotificationText - 1;
    // Never split a character outside the Basic Multilingual Plane.
    if (line.at(cut - 1).isHighSurrogate()) --cut;
    return line.left(cut) + QChar(0x2026);
}

NotificationPacer::NotificationPacer(qint64 cooldownMs) : cooldownMs_(cooldownMs) {}

bool NotificationPacer::offer(const DesktopNotification& notification, qint64 now) {
    const auto last = lastShown_.constFind(notification.key);
    if (last != lastShown_.cend() && now < *last + cooldownMs_) {
        waiting_.insert(notification.key, {notification, *last + cooldownMs_});
        return false;
    }
    lastShown_.insert(notification.key, now);
    waiting_.remove(notification.key);
    return true;
}

QList<DesktopNotification> NotificationPacer::takeDue(qint64 now) {
    QList<DesktopNotification> due;
    for (auto waiting = waiting_.begin(); waiting != waiting_.end();) {
        if (waiting->due > now) {
            ++waiting;
            continue;
        }
        due.append(waiting->notification);
        lastShown_.insert(waiting.key(), now);
        waiting = waiting_.erase(waiting);
    }
    return due;
}

std::optional<qint64> NotificationPacer::nextDue() const {
    std::optional<qint64> next;
    for (const Waiting& waiting : waiting_) {
        if (!next || waiting.due < *next) next = waiting.due;
    }
    return next;
}

void NotificationPacer::clear() {
    lastShown_.clear();
    waiting_.clear();
}
