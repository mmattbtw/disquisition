#pragma once

#include <QColor>
#include <QImage>
#include <QMap>
#include <QObject>
#include <QSizeF>
#include <QStringList>
#include <QTimer>
#include <functional>
#include <map>
#include <memory>

class QScreen;
class PenguinOverlayWindow;

// Activity belongs to the connection, not the overlay windows, so hiding the
// overlay or adding a display does not restart everyone's inactivity timeout.
class PenguinActivity {
public:
    void setMembers(const QStringList& names, qint64 nowMs);
    void noteActivity(const QString& name, qint64 nowMs);
    QStringList activeMembers(qint64 nowMs, int timeoutMinutes) const;

private:
    QMap<QString, qint64> lastActivity_;
};

struct DesktopPenguin {
    QColor color;
    double x = 0;
    double destination = 0;
    double idleSeconds = 0;
    double walkSeconds = 0;
    double speechSeconds = 0;
    int lane = 0;
    bool facingRight = false;
    QString speech;
};

// The scene has no native window or clock, so roster and movement can also be
// exercised without a desktop session. Positions are local display coordinates.
class PenguinScene {
public:
    void setMembers(QStringList names);
    void setColor(const QString& name, const QColor& color);
    void setSize(QSizeF size);
    void advance(double seconds, bool wandering);
    void showMessage(const QString& sender, const QString& body);
    const QMap<QString, DesktopPenguin>& penguins() const { return penguins_; }

private:
    QMap<QString, DesktopPenguin> penguins_;
    QSizeF size_ {800, 300};
};

// Owns one transparent, input-transparent window per display. It deliberately
// has no QWidget parent: minimizing the chat must not minimize the penguins.
class PenguinOverlay final : public QObject {
public:
    explicit PenguinOverlay(QObject* parent = nullptr, std::function<qint64()> now = {});
    ~PenguinOverlay() override;
    void setDisplayNames(const QMap<QString, QString>& names);
    void setMembers(QStringList names);
    void setColor(const QString& name, const QColor& color);
    void setEnabled(bool enabled);
    void setWandering(bool wandering);
    void setCustomSprites(const QString& name, const QImage& standing, const QImage& walking);
    void clearCustomSprites();
    void setIdleTimeoutMinutes(int minutes);
    void noteActivity(const QString& name);
    void showMessage(const QString& sender, const QString& body);

private:
    void syncScreens();
    QStringList members_;
    QMap<QString, QString> displayNames_;
    QMap<QString, QColor> colors_;
    PenguinActivity activity_;
    std::function<qint64()> now_;
    QTimer activityTimer_;
    QStringList visibleMembers_;
    int idleTimeoutMinutes_ = 10;
    bool enabled_ = true;
    bool wandering_ = true;
    QMap<QString, QPair<QImage, QImage>> sprites_;
    std::map<QScreen*, std::unique_ptr<PenguinOverlayWindow>> windows_;
};
