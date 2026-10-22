#include <QApplication>
#include <QComboBox>
#include <QEventLoop>
#include <QKeyEvent>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPushButton>
#include <QShortcut>
#include <QVBoxLayout>
#include <cstdio>

#include "check.h"
#include "desktop/screen_drawing_session.h"
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
    drawing.setColor(0);
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
    ScreenDrawingSession session("alice", "alice", &drawing, true);
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
    CHECK(drawing.enabled()); // the presenting client applies permission immediately
    CHECK(allow->isChecked() && requests == 1);
    drawing.setState(false, drawing.epoch() + 1);
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

void testEscapeFromPresenterWindows() {
    QWidget window;
    ScreenDrawing drawing;
    PresenterOverlay overlay(ShareSource{}, &drawing, &window);
    auto* tools = overlay.findChild<QWidget*>("screenDrawingToolbar");
    CHECK(tools);
    auto* control = tools->findChild<QPushButton*>();
    CHECK(control);
    drawing.setState(true, 1);
    drawing.setPenActive(true);
    QKeyEvent toolbarEscape(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
    QApplication::sendEvent(control, &toolbarEscape);
    CHECK(!drawing.penActive());
    drawing.setPenActive(true);
    QKeyEvent canvasEscape(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
    QApplication::sendEvent(&overlay, &canvasEscape);
    CHECK(!drawing.penActive());
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

void testClientOwnedDrawing() {
    ScreenDrawing own, viewer, late;
    ScreenDrawingSession host("alice", "alice", &own, true);
    ScreenDrawingSession guest("bob", "alice", &viewer, false);
    ScreenDrawingSession lateGuest("carol", "alice", &late, false);
    QObject::connect(&host, &ScreenDrawingSession::sendToPeer, &guest,
                     [&](const QString& target, const chat::Message& message) {
                         if (target == "bob")
                             guest.receive("alice", message);
                         else if (target == "carol")
                             lateGuest.receive("alice", message);
                         else
                             CHECK(false);
                     });
    QObject::connect(&guest, &ScreenDrawingSession::sendToPeer, &host,
                     [&](const QString& target, const chat::Message& message) {
                         CHECK(target == "alice");
                         host.receive("bob", message);
                     });
    QObject::connect(&lateGuest, &ScreenDrawingSession::sendToPeer, &host,
                     [&](const QString&, const chat::Message& message) {
                         host.receive("carol", message);
                     });
    guest.start();
    CHECK(!own.enabled() && !viewer.enabled());
    CHECK(viewer.status().isEmpty()); // the owner confirmed drawing is off
    own.permissionRequested(true);
    CHECK(own.enabled() && viewer.enabled()); // no server acknowledgement
    const auto epoch = own.epoch();
    CHECK(epoch > 0 && epoch == viewer.epoch());
    chat::ScreenDraw pen;
    pen.epoch = epoch;
    pen.id = 1;
    pen.x1 = 500;
    pen.x2 = 4000;
    pen.y1 = 100;
    pen.y2 = 5000;
    viewer.segmentDrawn(pen);
    CHECK(own.segments().size() == 1 && viewer.segments().size() == 1);
    CHECK(own.segments()[0].author == "bob");
    pen.sharer = "alice";
    // A nonparticipant's strokes and another peer's permission updates are ignored.
    host.receive("outsider", chat::screenDrawMessage(pen, false));
    guest.receive("outsider", {chat::MsgType::ScreenDrawState, {"alice", "0", "1"}});
    host.receive("bob", {chat::MsgType::ScreenDrawState, {"alice", "0", "1"}});
    CHECK(own.enabled() && viewer.enabled() && own.segments().size() == 1);
    // A duplicate proposal is not added or broadcast twice.
    host.receive("bob", chat::screenDrawMessage(pen, false));
    CHECK(own.segments().size() == 1);
    lateGuest.start();
    CHECK(late.enabled() && late.segments().size() == 1);
    pen.id = 2;
    own.segmentDrawn(pen);
    CHECK(own.segments().size() == 2 && viewer.segments().size() == 2 &&
          late.segments().size() == 2);
    CHECK(viewer.segments()[1].author == "alice");
    own.clearRequested();
    CHECK(own.enabled() && own.epoch() != epoch);
    CHECK(own.segments().isEmpty() && viewer.segments().isEmpty() && late.segments().isEmpty());
    host.receive("bob", chat::screenDrawMessage(pen, false)); // delayed pre-clear event
    CHECK(own.segments().isEmpty());
    guest.stop();
    pen.epoch = own.epoch();
    pen.id = 3;
    host.receive("bob", chat::screenDrawMessage(pen, false));
    CHECK(own.segments().isEmpty()); // unsubscribed viewers cannot draw
    own.segmentDrawn(pen);
    CHECK(late.segments().size() == 1 && viewer.segments().isEmpty());
    own.permissionRequested(false);
    CHECK(!own.enabled() && !late.enabled() && own.segments().isEmpty() &&
          late.segments().isEmpty());
    guest.start();
    CHECK(!viewer.enabled() && viewer.epoch() == own.epoch());
    own.permissionRequested(true);
    host.peerChanged("bob", false);
    own.clearRequested();
    guest.peerChanged("alice", true);
    CHECK(viewer.epoch() == own.epoch()); // link recovery resubscribes and resynchronizes
}

void testEditingAndIdentity() {
    ScreenDrawing owner, viewer, late;
    ScreenDrawingSession host("alice", "alice", &owner, true);
    ScreenDrawingSession guest("bob", "alice", &viewer, false);
    ScreenDrawingSession newcomer("carol", "alice", &late, false);
    QObject::connect(&host, &ScreenDrawingSession::sendToPeer, &guest,
                     [&](const QString& peer, const chat::Message& message) {
                         if (peer == "bob") guest.receive("alice", message);
                         if (peer == "carol") newcomer.receive("alice", message);
                     });
    QObject::connect(&guest, &ScreenDrawingSession::sendToPeer, &host,
                     [&](const QString&, const chat::Message& message) { host.receive("bob", message); });
    QObject::connect(&newcomer, &ScreenDrawingSession::sendToPeer, &host,
                     [&](const QString&, const chat::Message& message) { host.receive("carol", message); });
    viewer.setChatColor(QColor("#aabbcc"));
    CHECK(viewer.penColor() == QColor("#aabbcc"));
    viewer.setColor(3);
    CHECK(viewer.penColor() == screenPenColor(3));
    viewer.setChatColor(QColor("#123456"));
    CHECK(viewer.penColor() == screenPenColor(3)); // a chosen color stays chosen
    viewer.setColor(-1);
    CHECK(viewer.penColor() == QColor("#123456"));
    guest.start();
    owner.permissionRequested(true);
    chat::ScreenDraw pen;
    pen.id = 1; pen.stroke = 1; pen.rgb = 0x123456;
    viewer.segmentDrawn(pen);
    pen.id = 2;
    viewer.segmentDrawn(pen);
    pen.id = 3; pen.stroke = 3;
    owner.segmentDrawn(pen);
    CHECK(owner.segments().size() == 3 && viewer.segments().size() == 3);
    CHECK(owner.activeAuthors().size() == 2);
    CHECK(owner.segments()[0].rgb == 0x123456);
    // A viewer cannot erase the presenter's stroke, even by bypassing the controls.
    guest.receive("mallory", {chat::MsgType::ScreenDrawErase,
                              {"alice", std::to_string(owner.epoch()), "alice", "3"}});
    host.receive("bob", {chat::MsgType::ScreenDrawErase,
                        {"alice", std::to_string(owner.epoch()), "alice", "3"}});
    CHECK(owner.segments().size() == 3);
    viewer.eraseRequested("bob", {1});
    CHECK(owner.segments().size() == 1 && viewer.segments().size() == 1);
    // An in-flight segment from an erased gesture must not resurrect the stroke.
    pen.id = 4; pen.stroke = 1;
    viewer.segmentDrawn(pen);
    CHECK(owner.segments().size() == 1 && viewer.segments().size() == 1);
    pen.id = 5; pen.stroke = 5;
    viewer.segmentDrawn(pen);
    CHECK(owner.segments().size() == 2);
    viewer.clearMineRequested();
    CHECK(owner.segments().size() == 1 && owner.segments()[0].author == "alice");
    newcomer.start();
    CHECK(late.segments().size() == 1); // replay excludes erased strokes
    owner.eraseRequested("alice", {3});
    CHECK(owner.segments().isEmpty() && viewer.segments().isEmpty() && late.segments().isEmpty());
    CHECK(owner.activeAuthors().isEmpty());
    pen.id = 6; pen.stroke = 6;
    viewer.segmentDrawn(pen);
    pen.id = 7; pen.stroke = 7;
    owner.segmentDrawn(pen);
    owner.clearMineRequested();
    CHECK(owner.segments().size() == 1 && owner.segments()[0].author == "bob");
    owner.eraseRequested("bob", {6}); // presenter may remove somebody else's stroke
    CHECK(owner.segments().isEmpty() && late.segments().isEmpty());
    const auto oldEpoch = owner.epoch();
    owner.clearRequested();
    pen.id = 8; pen.stroke = 8;
    viewer.segmentDrawn(pen);
    host.receive("bob", {chat::MsgType::ScreenDrawErase, {"alice", std::to_string(oldEpoch), "bob", "8"}});
    host.receive("outsider", {chat::MsgType::ScreenDrawErase, {"alice", std::to_string(owner.epoch()), "bob", "8"}});
    CHECK(owner.segments().size() == 1);
    viewer.requestErase({screenStrokeKey(owner.segments().front())});
    CHECK(owner.segments().isEmpty());
    viewer.setTool(ScreenDrawing::Tool::Select);
    CHECK(viewer.tool() != ScreenDrawing::Tool::Select);
}

void testEraserAndSelection() {
    ScreenDrawing drawing;
    ScreenDrawingSession host("alice", "alice", &drawing, true);
    drawing.permissionRequested(true);
    DrawingCanvas canvas;
    canvas.resize(400, 200);
    canvas.setDrawing(&drawing);
    drawing.setPenActive(true);
    mouse(canvas, QEvent::MouseButtonPress, {40, 60}, Qt::LeftButton, Qt::LeftButton);
    mouse(canvas, QEvent::MouseButtonRelease, {360, 60}, Qt::LeftButton, Qt::NoButton);
    CHECK(drawing.segments().size() == 2);
    CHECK(drawing.segments()[0].stroke == drawing.segments()[1].stroke);
    drawing.setTool(ScreenDrawing::Tool::Eraser);
    // Scrubbing across a line erases the whole stroke, even if endpoints miss it.
    mouse(canvas, QEvent::MouseButtonPress, {200, 20}, Qt::LeftButton, Qt::LeftButton);
    mouse(canvas, QEvent::MouseButtonRelease, {200, 100}, Qt::LeftButton, Qt::NoButton);
    CHECK(drawing.segments().isEmpty());
    drawing.setPenActive(true);
    mouse(canvas, QEvent::MouseButtonPress, {20, 80}, Qt::LeftButton, Qt::LeftButton);
    mouse(canvas, QEvent::MouseButtonRelease, {380, 80}, Qt::LeftButton, Qt::NoButton);
    drawing.setTool(ScreenDrawing::Tool::Select);
    // A selection box intersects the middle, rather than containing the endpoints.
    mouse(canvas, QEvent::MouseButtonPress, {180, 60}, Qt::LeftButton, Qt::LeftButton);
    mouse(canvas, QEvent::MouseButtonRelease, {220, 100}, Qt::LeftButton, Qt::NoButton);
    CHECK(drawing.selectedStrokes().size() == 1);
    drawing.deleteSelection();
    CHECK(drawing.segments().isEmpty());
    drawing.setPenActive(true);
    click(canvas, {300, 140}); // a dot is also a selectable stroke
    drawing.setTool(ScreenDrawing::Tool::Select);
    click(canvas, {302, 142});
    CHECK(drawing.selectedStrokes().size() == 1);
    QKeyEvent remove(QEvent::KeyPress, Qt::Key_Delete, Qt::NoModifier);
    QApplication::sendEvent(&canvas, &remove);
    CHECK(drawing.segments().isEmpty());
}

void testAttributionAndPalette() {
    ScreenDrawing drawing;
    ScreenDrawingSession session("alice", "alice", &drawing, true);
    drawing.permissionRequested(true);
    DrawingControls controls(&drawing, true);
    const auto palette = controls.findChild<QComboBox*>();
    CHECK(palette && palette->currentText() == "chat color");
    drawing.setChatColor(QColor("#abcdef"));
    CHECK(drawing.penColor() == QColor("#abcdef"));
    palette->setCurrentIndex(2); // yellow, following chat color and red
    DrawingCanvas canvas;
    canvas.resize(400, 200);
    canvas.setDrawing(&drawing);
    drawing.setPenActive(true);
    click(canvas, {100, 100});
    CHECK(drawing.segments().size() == 1);
    CHECK(screenSegmentColor(drawing.segments()[0]) == screenPenColor(1));
    CHECK(drawing.activeAuthors().size() == 1 && drawing.activeAuthors()[0].author == "alice");
    CHECK(drawing.activeAuthors()[0].x2 == 2500);
    QEventLoop loop;
    QTimer::singleShot(2100, &loop, &QEventLoop::quit);
    loop.exec();
    CHECK(drawing.activeAuthors().isEmpty());
    CHECK(drawing.segments().size() == 1); // only the activity label expires
}

void testDrawingHistoryLimitAndMissingPeer() {
    ScreenDrawing own;
    ScreenDrawingSession host("alice", "alice", &own, true);
    own.permissionRequested(true);
    const auto epoch = own.epoch();
    chat::ScreenDraw segment;
    for (std::size_t i = 0; i <= chat::kMaxDrawSegments; ++i) {
        segment.id = static_cast<std::uint32_t>(i);
        own.segmentDrawn(segment);
    }
    CHECK(own.enabled() && own.epoch() != epoch && own.segments().isEmpty());
    const auto beforeChurn = own.epoch();
    for (std::size_t i = 0; i <= chat::kMaxDrawSegments; ++i) {
        segment.id = static_cast<std::uint32_t>(i + chat::kMaxDrawSegments + 1);
        own.segmentDrawn(segment);
        own.clearMineRequested();
    }
    CHECK(own.epoch() != beforeChurn && own.historySize() == 0);
    ScreenDrawing viewer;
    ScreenDrawingSession guest("bob", "alice", &viewer, false);
    guest.start(); // no compatible client/relay responds
    CHECK(!viewer.status().isEmpty());
    guest.expireRequest();
    CHECK(!viewer.enabled() && viewer.status().contains("unavailable"));
    // An unrelated peer cannot acknowledge the request or enable the pen.
    guest.receive("mallory", {chat::MsgType::ScreenDrawState, {"alice", "1", "42"}});
    CHECK(!viewer.enabled());
}

} // namespace

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    testPermissionsAndLocalEcho();
    testLetterboxingAndResize();
    testDragAndStopWatching();
    testAccessibleControls();
    testEscapeFromAnotherControl();
    testEscapeFromPresenterWindows();
    testClosingFullscreenRestoresEscape();
    testClientOwnedDrawing();
    testEditingAndIdentity();
    testEraserAndSelection();
    testAttributionAndPalette();
    testDrawingHistoryLimitAndMissingPeer();
    std::puts("screen_drawing_test: ok");
}
