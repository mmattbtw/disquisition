#pragma once

#include <QImage>
#include <QObject>
#include <QString>
#include <memory>

// Toasts in the Windows notification center.
class WindowsNotifications final : public QObject {
    Q_OBJECT

public:
    explicit WindowsNotifications(const QImage& icon, QObject* parent = nullptr);
    ~WindowsNotifications() override;

    bool isAvailable() const;
    void show(const QString& key, const QString& title, const QString& body);
    void removeAll();

signals:
    void activated();

private:
    struct State;
    std::unique_ptr<State> state_;
};
