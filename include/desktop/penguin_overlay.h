#pragma once

#include <QColor>
#include <QMap>
#include <QObject>
#include <QSizeF>
#include <QStringList>
#include <map>
#include <memory>

class QScreen;
class PenguinOverlayWindow;

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
    explicit PenguinOverlay(QObject* parent = nullptr);
    ~PenguinOverlay() override;
    void setMembers(QStringList names);
    void setColor(const QString& name, const QColor& color);
    void setEnabled(bool enabled);
    void setWandering(bool wandering);
    void showMessage(const QString& sender, const QString& body);

private:
    void syncScreens();
    QStringList members_;
    QMap<QString, QColor> colors_;
    bool enabled_ = true;
    bool wandering_ = true;
    std::map<QScreen*, std::unique_ptr<PenguinOverlayWindow>> windows_;
};
