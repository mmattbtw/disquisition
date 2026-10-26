#pragma once

#include <QString>

#include <functional>

// Outside an app bundle only the Dock badge works.
class MacNotifications {
public:
    // `activated` runs on the main thread.
    explicit MacNotifications(std::function<void()> activated);
    ~MacNotifications();
    MacNotifications(const MacNotifications&) = delete;
    MacNotifications& operator=(const MacNotifications&) = delete;

    void show(const QString& key, const QString& title, const QString& body);
    void removeAll();
    void setBadgeLabel(const QString& label);

private:
    struct State;
    State* state_;
};
