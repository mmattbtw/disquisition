#pragma once

#include <QElapsedTimer>
#include <QPointer>
#include <QSet>
#include <QTimer>
#include <QWidget>
#include <optional>

#include "common/protocol.h"
#include "desktop/share_source.h"

class QPainter;

// One drawing shared by the tile, fullscreen view and presenter's overlay.
class ScreenDrawing final : public QObject {
    Q_OBJECT
public:
    explicit ScreenDrawing(QObject* parent = nullptr) : QObject(parent) {}
    void setState(bool enabled, std::int64_t epoch);
    void add(const chat::ScreenDraw& segment);
    void setPenActive(bool active);
    void setColor(int color);
    void setPresenterOverlayAvailable(bool available);
    bool presenterOverlayAvailable() const { return presenterOverlayAvailable_; }
    bool enabled() const { return enabled_; }
    bool penActive() const { return penActive_; }
    int color() const { return color_; }
    std::int64_t epoch() const { return epoch_; }
    const QList<chat::ScreenDraw>& segments() const { return segments_; }
    std::uint32_t nextSegmentId() { return ++nextSegmentId_; }

signals:
    void changed();
    void updated();
    void segmentDrawn(const chat::ScreenDraw& segment);
    void permissionRequested(bool enabled);
    void clearRequested();

private:
    bool enabled_ = false;
    bool penActive_ = false;
    bool presenterOverlayAvailable_ = true;
    int color_ = 0;
    std::int64_t epoch_ = 0;
    QList<chat::ScreenDraw> segments_;
    QSet<QString> received_;
    std::uint32_t nextSegmentId_ = 0;
};

class DrawingControls final : public QWidget {
    Q_OBJECT
public:
    DrawingControls(ScreenDrawing* drawing, bool owner, QWidget* parent = nullptr);
};

// Coordinates always refer to the video content, excluding letterboxing.
class DrawingCanvas : public QWidget {
    Q_OBJECT
public:
    explicit DrawingCanvas(QWidget* parent = nullptr);
    void setDrawing(ScreenDrawing* drawing);
    ScreenDrawing* drawing() const { return drawing_; }

protected:
    virtual QRectF drawingRect() const { return rect(); }
    void paintDrawing(QPainter& painter);
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;

private:
    void drawTo(const QPointF& position, bool first = false);
    QPointer<ScreenDrawing> drawing_;
    std::optional<QPoint> previous_;
    QElapsedTimer penClock_;
};

// A transparent top-level canvas follows the capture source. Its separate
// toolbar stays clickable when the canvas passes input through to the app.
class PresenterOverlay final : public DrawingCanvas {
    Q_OBJECT
public:
    PresenterOverlay(const ShareSource& source, ScreenDrawing* drawing, QWidget* parent);
    ~PresenterOverlay() override;

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    void refresh();
    ShareSource source_;
    QWidget* tools_ = nullptr;
    QTimer positionTimer_;
};

QColor screenPenColor(int index);
