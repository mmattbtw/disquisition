#include <QApplication>
#include <QKeyEvent>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPushButton>
#include <QShortcut>
#include <QVBoxLayout>
#include <cstdio>

#include "check.h"
#include "desktop/screen_stage.h"

namespace {

void mouse(QWidget& view, QEvent::Type type, QPointF position, Qt::MouseButton button,
           Qt::MouseButtons buttons) {
    QMouseEvent event(type, position, view.mapToGlobal(position), button, buttons, Qt::NoModifier);
    QApplication::sendEvent(&view, &event);
}

void click(QWidget& view, QPointF position) {
    mouse(view, QEvent::MouseButtonPress, position, Qt::LeftButton, Qt::LeftButton);
    mouse(view, QEvent::MouseButtonRelease, position, Qt::LeftButton, Qt::NoButton);
}

void testPermissionsAndLocalEcho() {
    ScreenDrawing drawing;
    drawing.setPenActive(true);
    CHECK(!drawing.penActive());
    drawing.setState(true, 10);
    drawing.setPenActive(true);
    CHECK(drawing.penActive());
    chat::ScreenDraw segment;
    segment.epoch = 10;
    segment.author = "alice";
    segment.id = 1;
    drawing.add(segment);
    drawing.add(segment); // server echo of an optimistic local stroke
    CHECK(drawing.segments().size() == 1);
    segment.author = "bob";
    drawing.add(segment); // different people can use the same segment ID
    CHECK(drawing.segments().size() == 2);
    drawing.setState(true, 11);
    CHECK(drawing.segments().isEmpty() && drawing.penActive());
    drawing.add(segment); // delayed pre-clear event
    CHECK(drawing.segments().isEmpty());
    segment.epoch = 11;
    drawing.add(segment);
    CHECK(drawing.segments().size() == 1);
    drawing.setState(false, 12);
    CHECK(!drawing.penActive() && drawing.segments().isEmpty());
    segment.epoch = 12;
    drawing.add(segment);
    CHECK(drawing.segments().isEmpty());
}

void testLetterboxingAndResize() {
    ScreenDrawing drawing;
    StreamView view;
    view.setDrawing(&drawing);
    view.resize(400, 400);
    QImage image(400, 200, QImage::Format_RGB32);
    image.fill(Qt::black);
    view.setImage(image);
    QList<chat::ScreenDraw> sent;
    QObject::connect(&drawing, &ScreenDrawing::segmentDrawn, &drawing,
                     [&](const chat::ScreenDraw& segment) { sent.append(segment); });
    click(view, {100, 200});
    CHECK(sent.isEmpty()); // disabled
    drawing.setState(true, 1);
    drawing.setPenActive(true);
    click(view, {100, 50});
    CHECK(sent.isEmpty()); // letterbox
    click(view, {100, 200});
    CHECK(sent.size() == 1);
    CHECK(sent[0].x1 == 2500 && sent[0].y1 == 5000);
    CHECK(sent[0].x2 == 2500 && sent[0].y2 == 5000);
    CHECK(sent[0].epoch == 1);
    drawing.add(sent[0]);
    const auto pixel = [](QWidget& widget, int x, int y) {
        const QImage rendered = widget.grab().toImage();
        const qreal scale = rendered.devicePixelRatio();
        return rendered.pixelColor(qRound(x * scale), qRound(y * scale));
    };
    // A small antialiased dot blends with the black video at pixel edges.
    const auto red = [](QColor color) {
        return color.red() > color.green() + 40 && color.red() > color.blue() + 40;
    };
    CHECK(red(pixel(view, 100, 200)));
    CHECK(!red(pixel(view, 100, 50)));
    view.resize(800, 400);
    CHECK(red(pixel(view, 200, 200)));
    // Fullscreen and tiled views read the same drawing with their own sizes.
    StreamView second;
    second.setDrawing(&drawing);
    second.setImage(image);
    second.resize(800, 400);
    CHECK(red(pixel(second, 200, 200)));
    drawing.setState(true, 2);
    CHECK(pixel(view, 200, 200) == QColor(Qt::black));
    CHECK(pixel(second, 200, 200) == QColor(Qt::black));
    // There is no drawable area before the first frame arrives.
    view.clear();
    click(view, {100, 200});
    CHECK(sent.size() == 1);
}

void testDragAndStopWatching() {
    ScreenDrawing drawing;
    StreamView view;
    view.setDrawing(&drawing);
    view.resize(400, 200);
    QImage image(400, 200, QImage::Format_RGB32);
    image.fill(Qt::black);
    view.setImage(image);
    drawing.setState(true, 7);
    drawing.setPenActive(true);
    QList<chat::ScreenDraw> sent;
    QObject::connect(&drawing, &ScreenDrawing::segmentDrawn, &drawing,
                     [&](const chat::ScreenDraw& segment) { sent.append(segment); });
    mouse(view, QEvent::MouseButtonPress, {40, 100}, Qt::LeftButton, Qt::LeftButton);
    mouse(view, QEvent::MouseButtonRelease, {360, 100}, Qt::LeftButton, Qt::NoButton);
    CHECK(sent.size() == 2);
    CHECK(sent[1].x1 == 1000 && sent[1].x2 == 9000 && sent[1].y2 == 5000);
    CHECK(sent[0].id != sent[1].id);
    StreamTile tile("alice", false);
    tile.setWatching(true);
    tile.drawing()->setState(true, 1);
    tile.drawing()->setPenActive(true);
    tile.setWatching(false);
    CHECK(!tile.drawing()->enabled() && !tile.drawing()->penActive());
}

void testAccessibleControls() {
    ScreenDrawing drawing;
    DrawingControls controls(&drawing, true);
    QPushButton* allow = nullptr;
    for (auto* button : controls.findChildren<QPushButton*>()) {
        if (button->text() == "allow drawing")
            allow = button;
    }
    CHECK(allow);
    int requests = 0;
    QObject::connect(&drawing, &ScreenDrawing::permissionRequested, &drawing, [&](bool enabled) {
        ++requests;
        CHECK(enabled);
    });
    // Qt's accessible toggle action changes the checked state directly.
    allow->setChecked(true);
    CHECK(requests == 1);
    CHECK(!drawing.enabled()); // waits for the server's permission state
    drawing.setState(true, 1);
    CHECK(allow->isChecked() && requests == 1); // no request loop on server updates
    drawing.setState(false, 2);
    CHECK(!allow->isChecked() && requests == 1);
}

void testEscapeFromAnotherControl() {
    QWidget window;
    auto* layout = new QVBoxLayout(&window);
    auto* stage = new ScreenStage(&window);
    auto* composer = new QLineEdit(&window);
    layout->addWidget(stage);
    layout->addWidget(composer);
    stage->addStream("alice", true);
    stage->drawing("alice")->setState(true, 1);
    stage->drawing("alice")->setPenActive(true);
    window.show();
    window.activateWindow();
    composer->setFocus();
    QApplication::processEvents();
    QKeyEvent escape(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
    QApplication::sendEvent(composer, &escape);
    CHECK(!stage->drawing("alice")->penActive());
}

void testClosingFullscreenRestoresEscape() {
    QWidget window;
    auto* layout = new QVBoxLayout(&window);
    auto* stage = new ScreenStage(&window);
    layout->addWidget(stage);
    stage->addStream("alice", true);
    window.show();
    for (auto* button : stage->findChildren<QPushButton*>()) {
        if (button->text() == "fullscreen")
            button->click();
    }
    QPointer<QWidget> fullscreen;
    for (auto* widget : QApplication::topLevelWidgets()) {
        if (widget->windowTitle() == "alice's screen")
            fullscreen = widget;
    }
    CHECK(fullscreen);
    CHECK(stage->findChild<QShortcut*>()->isEnabled());
    fullscreen->close(); // the system window controls, rather than our Esc handler
    QApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    QApplication::processEvents();
    CHECK(!fullscreen);
    // Stop intercepting Escape when the system closes the fullscreen window.
    CHECK(!stage->findChild<QShortcut*>()->isEnabled());
}

} // namespace

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    testPermissionsAndLocalEcho();
    testLetterboxingAndResize();
    testDragAndStopWatching();
    testAccessibleControls();
    testEscapeFromAnotherControl();
    testClosingFullscreenRestoresEscape();
    std::puts("screen_drawing_test: ok");
}
