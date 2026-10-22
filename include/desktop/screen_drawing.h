#pragma once

#include <QColor>
#include <QElapsedTimer>
#include <QHash>
#include <QPointer>
#include <QSet>
#include <QTimer>
#include <QWidget>
#include <optional>

#include "common/protocol.h"
#include "desktop/share_source.h"

class QPainter;
#if defined(Q_OS_MACOS)
class MacDrawingPanel;
#endif

// One drawing shared by the tile, fullscreen view and presenter's overlay.
class ScreenDrawing final : public QObject {
    Q_OBJECT
public:
    enum class Tool { None, Pen, Eraser, Select };
    explicit ScreenDrawing(QObject* parent = nullptr);
    void setIdentity(const QString& name, bool owner);
    void setChatColor(const QColor& color);
    QColor chatColor() const { return chatColor_; }
    QColor penColor() const;
    QString localName() const { return localName_; }
    bool owner() const { return owner_; }
    void setTool(Tool tool);
    Tool tool() const { return tool_; }
    bool canErase(const chat::ScreenDraw& segment) const;
    void remove(const QString& author, const QList<quint32>& strokes);
    QList<quint32> strokeIds(const QString& author) const;
    void requestErase(const QSet<QString>& strokes);
    void select(const QSet<QString>& strokes);
    const QSet<QString>& selectedStrokes() const { return selected_; }
    void deleteSelection();
    void setSelectionBox(const QRectF& box);
    QRectF selectionBox() const { return selectionBox_; }
    QList<chat::ScreenDraw> activeAuthors() const;
    qsizetype historySize() const { return received_.size(); }
    void setState(bool enabled, std::int64_t epoch);
    void add(const chat::ScreenDraw& segment);
    void setPenActive(bool active);
    void setColor(int color);
    void setPresenterOverlayAvailable(bool available);
    void setStatus(const QString& status);
    QString status() const { return status_; }
    bool presenterOverlayAvailable() const { return presenterOverlayAvailable_; }
    bool enabled() const { return enabled_; }
    bool penActive() const { return tool_ != Tool::None; }
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
    void clearMineRequested();
    void eraseRequested(const QString& author, const QList<quint32>& strokes);

private:
    bool enabled_ = false;
    Tool tool_ = Tool::None;
    QString localName_;
    bool owner_ = false;
    QColor chatColor_{"#87ffaf"};
    QSet<QString> removed_;
    QSet<QString> selected_;
    QRectF selectionBox_;
    struct ActiveAuthor { chat::ScreenDraw segment; qint64 seen = 0; };
    QHash<QString, ActiveAuthor> activeAuthors_;
    QElapsedTimer activityClock_;
    QTimer activityTimer_;
    bool presenterOverlayAvailable_ = true;
    QString status_;
    int color_ = -1;
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
    // Native presenter hosts use the same painting and input logic.
    void renderDrawing(QPainter& painter);

protected:
    virtual QRectF drawingRect() const { return rect(); }
    void paintDrawing(QPainter& painter);
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;

private:
    void drawTo(const QPointF& position, bool first = false);
    void eraseTo(const QPointF& position);
    void selectTo(const QPointF& position, bool finished);
    QPointF normalized(const QPointF& position) const;
    std::optional<QPointF> gestureStart_;
    std::optional<QPointF> eraserPrevious_;
    quint32 stroke_ = 0;
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
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void refresh();
    ShareSource source_;
    QWidget* tools_ = nullptr;
    QTimer positionTimer_;
#if defined(Q_OS_MACOS)
    MacDrawingPanel* canvasPanel_ = nullptr;
    MacDrawingPanel* toolsPanel_ = nullptr;
#endif
};

QColor screenPenColor(int index);
QColor screenSegmentColor(const chat::ScreenDraw& segment);
QString screenStrokeKey(const chat::ScreenDraw& segment);
