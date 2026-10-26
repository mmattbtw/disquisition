#pragma once

#include <QObject>
#include <QRect>
#include <memory>

class QWidget;
class ScreenDrawing;

// Hosts Qt drawing controls in a native panel that leaves the shared app active.
class MacDrawingPanel final : public QObject {
public:
    explicit MacDrawingPanel(QWidget* widget, ScreenDrawing* drawing = nullptr);
    ~MacDrawingPanel() override;
    void show(const QRect& bounds, bool transparent = false);
    void hide();
    void update();

private:
    struct Native;
    std::unique_ptr<Native> native_;
    QWidget* widget_;
};
