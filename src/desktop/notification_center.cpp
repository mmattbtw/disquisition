#include "desktop/notification_center.h"

#include <QAction>
#include <QApplication>
#include <QEvent>
#include <QSettings>
#include <QSignalBlocker>
#include <QWidget>
#include <algorithm>

#include "desktop/desktop_notifier.h"

NotificationCenter::NotificationCenter(QWidget* window) :
    QObject(window), window_(window), settings_(NotificationSettings::load(QSettings())) {
    muteAction_ = new QAction("Mute notifications", this);
    muteAction_->setCheckable(true);
    muteAction_->setChecked(settings_.muted);
    connect(muteAction_, &QAction::toggled, this, [this](bool muted) {
        NotificationSettings changed = settings_;
        changed.muted = muted;
        setSettings(changed);
    });

    auto* open = new QAction("Open " + QApplication::applicationDisplayName(), this);
    connect(open, &QAction::triggered, this, &NotificationCenter::bringWindowToFront);
    auto* separator = new QAction(this);
    separator->setSeparator(true);
    auto* quit = new QAction("Quit", this);
    connect(quit, &QAction::triggered, window_, &QWidget::close);
    notifier_ = new DesktopNotifier({open, muteAction_, separator, quit}, this);
    connect(notifier_, &DesktopNotifier::activated, this, &NotificationCenter::bringWindowToFront);

    clock_.start();
    pacerTimer_.setSingleShot(true);
    connect(&pacerTimer_, &QTimer::timeout, this, &NotificationCenter::showDueNotifications);

    connect(qApp, &QGuiApplication::applicationStateChanged, this,
            &NotificationCenter::markReadIfInView);
    window_->installEventFilter(this);
}

NotificationCenter::~NotificationCenter() = default;

void NotificationCenter::setSettings(const NotificationSettings& settings) {
    if (settings == settings_) return;
    settings_ = settings;
    QSettings stored;
    settings_.save(stored);
    // What waits was let through by the old settings.
    pacer_.clear();
    pacerTimer_.stop();
    const QSignalBlocker blocker(muteAction_);
    muteAction_->setChecked(settings_.muted);
    emit settingsChanged(settings_);
}

void NotificationCenter::messageReceived(const QString& sender, const QString& text,
                                         Mentioned mentioned) {
    if (windowInView()) return;
    const MessageAttention attention = attentionFor(settings_, mentioned);
    UnreadBadge badge = badge_;
    badge.unread = true;
    if (attention.mention) ++badge.mentions;
    setBadge(badge);
    if (attention.notify) notify("message/" + sender, sender, text);
    if (attention.alert) notifier_->requestAttention(window_);
}

void NotificationCenter::activityOccurred(const QString& member, const QString& text) {
    if (!windowInView() && notifiesActivity(settings_)) notify("activity/" + member, member, text);
}

bool NotificationCenter::eventFilter(QObject* watched, QEvent* event) {
    if (watched == window_ &&
        (event->type() == QEvent::WindowStateChange || event->type() == QEvent::WindowActivate ||
         event->type() == QEvent::Show)) {
        markReadIfInView();
    }
    return QObject::eventFilter(watched, event);
}

// Not isActiveWindow(): a dialog of the window may have the focus.
bool NotificationCenter::windowInView() const {
    return QGuiApplication::applicationState() == Qt::ApplicationActive &&
           window_->isVisible() && !window_->isMinimized();
}

void NotificationCenter::markReadIfInView() {
    if (!windowInView()) return;
    setBadge({});
    pacer_.clear();
    pacerTimer_.stop();
    if (notified_) {
        notified_ = false;
        notifier_->withdrawAll();
    }
}

void NotificationCenter::setBadge(const UnreadBadge& badge) {
    if (badge == badge_) return;
    badge_ = badge;
    notifier_->setBadge(badge_);
    emit badgeChanged(badge_);
}

void NotificationCenter::notify(const QString& key, const QString& title, const QString& text) {
    const DesktopNotification notification{key, title, notificationText(text)};
    if (pacer_.offer(notification, clock_.elapsed())) {
        show(notification);
    } else {
        schedulePacedNotifications();
    }
}

void NotificationCenter::show(const DesktopNotification& notification) {
    notifier_->show(notification);
    notified_ = true;
}

void NotificationCenter::showDueNotifications() {
    for (const DesktopNotification& notification : pacer_.takeDue(clock_.elapsed())) {
        show(notification);
    }
    schedulePacedNotifications();
}

void NotificationCenter::schedulePacedNotifications() {
    const std::optional<qint64> due = pacer_.nextDue();
    if (!due) return;
    pacerTimer_.start(static_cast<int>(std::max<qint64>(0, *due - clock_.elapsed())));
}

void NotificationCenter::bringWindowToFront() {
    window_->setWindowState((window_->windowState() & ~Qt::WindowMinimized) | Qt::WindowActive);
    window_->show();
    window_->raise();
    window_->activateWindow();
}
