#pragma once

#include <QHash>
#include <QImage>
#include <QObject>
#include <QString>

class FreedesktopNotifications final : public QObject {
    Q_OBJECT

public:
    explicit FreedesktopNotifications(const QImage& icon, QObject* parent = nullptr);

    void show(const QString& key, const QString& title, const QString& body);
    void closeAll();

signals:
    void activated();
    // Emitted at most once, from the event loop.
    void unavailable();

private slots:
    void actionInvoked(uint id, const QString& action);
    void activationToken(uint id, const QString& token);
    void notificationClosed(uint id, uint reason);

private:
    void probe();
    bool owns(uint id) const;

    QImage icon_;
    // Assumed until the server's capabilities say otherwise.
    bool bodyMarkup_ = true;
    bool actions_ = true;
    QHash<QString, uint> showing_;
};
