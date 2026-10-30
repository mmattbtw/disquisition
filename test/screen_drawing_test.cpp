#include <QApplication>
#include <QLineEdit>
#include <QColorDialog>
#include <QEventLoop>
#include <QKeyEvent>
#include <QLineEdit>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
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
        if (button->text() == "Fullscreen")
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

void testUndoOwnStrokes() {
    ScreenDrawing own, viewer;
    ScreenDrawingSession host("alice", "alice", &own, true);
    ScreenDrawingSession guest("bob", "alice", &viewer, false);
    QObject::connect(&host, &ScreenDrawingSession::sendToPeer, &guest,
                     [&](const QString&, const chat::Message& message) { guest.receive("alice", message); });
    QObject::connect(&guest, &ScreenDrawingSession::sendToPeer, &host,
                     [&](const QString&, const chat::Message& message) { host.receive("bob", message); });
    guest.start();
    own.permissionRequested(true);
    chat::ScreenDraw stroke;
    stroke.id = 1; stroke.stroke = 1;
    viewer.segmentDrawn(stroke);
    stroke.id = 2;
    viewer.segmentDrawn(stroke); // two segments, one gesture
    stroke.id = 3; stroke.stroke = 3;
    viewer.segmentDrawn(stroke);
    stroke.id = 4; stroke.stroke = 4;
    own.segmentDrawn(stroke); // another author drew more recently
    CHECK(viewer.canUndo() && own.canUndo());
    DrawingCanvas canvas;
    canvas.setDrawing(&viewer);
    QKeyEvent undo(QEvent::KeyPress, Qt::Key_Z, Qt::ControlModifier);
    QApplication::sendEvent(&canvas, &undo);
    CHECK(own.segments().size() == 3 && viewer.segments().size() == 3);
    CHECK(viewer.strokeIds("bob") == QList<quint32>{1});
    CHECK(own.strokeIds("alice") == QList<quint32>{4});
    QApplication::sendEvent(&canvas, &undo);
    CHECK(own.segments().size() == 1 && viewer.segments().size() == 1);
    CHECK(!viewer.canUndo());
    viewer.undo();
    CHECK(own.segments().size() == 1); // no other person's stroke is removed
    host.setBanned("bob", true);
    CHECK(!viewer.canUndo());
    own.undo();
    CHECK(own.segments().isEmpty() && viewer.segments().isEmpty());
    own.undo();
    CHECK(!own.canUndo());
    own.permissionRequested(false);
    CHECK(!own.canUndo());
}

void testHexColorsAndPicker() {
    ScreenDrawing drawing;
    ScreenDrawingSession host("alice", "alice", &drawing, true);
    drawing.permissionRequested(true);
    DrawingControls controls(&drawing, true);
    auto* editor = controls.findChild<QLineEdit*>("drawingHexColor");
    auto* custom = controls.findChild<QPushButton*>("drawingSwatch7");
    CHECK(editor && custom && editor->isHidden()); // the hex field belongs to the custom swatch
    custom->click();
    CHECK(custom->isChecked() && !editor->isHidden());
    editor->selectAll();
    editor->insert("#1a2b3c");
    CHECK(drawing.penColor() == QColor("#1a2b3c"));
    editor->editingFinished();
    CHECK(editor->text() == "#1A2B3C");
    editor->selectAll();
    editor->insert("deadbe");
    CHECK(drawing.penColor() == QColor("#deadbe"));
    editor->editingFinished();
    CHECK(editor->text() == "#DEADBE");
    editor->selectAll();
    editor->insert("#xyz");
    editor->editingFinished();
    CHECK(drawing.penColor() == QColor("#deadbe") && editor->text() == "#DEADBE");
    drawing.setChatColor(QColor("#abcdef"));
    CHECK(drawing.penColor() == QColor("#deadbe")); // custom color survives chat color changes
    DrawingCanvas canvas;
    canvas.resize(400, 200);
    canvas.setDrawing(&drawing);
    drawing.setPenActive(true);
    click(canvas, {100, 100});
    CHECK(drawing.segments().size() == 1);
    const auto& ink = drawing.segments().last();
    CHECK(ink.rgb == 0xdeadbe);
    chat::ScreenDraw decoded;
    CHECK(chat::parseScreenDraw(chat::screenDrawMessage(ink, true), true, decoded));
    CHECK(screenSegmentColor(decoded) == QColor("#deadbe"));
    auto* pick = controls.findChild<QPushButton*>("drawingPickColor");
    CHECK(pick);
    pick->click();
    auto* picker = controls.findChild<QColorDialog*>("screenDrawingColorPicker");
    CHECK(picker && picker->isVisible() && picker->testOption(QColorDialog::DontUseNativeDialog));
    picker->setCurrentColor(QColor("#123456"));
    CHECK(drawing.penColor() == QColor("#deadbe")); // choosing a draft doesn't change ink color
    picker->reject();
    CHECK(drawing.penColor() == QColor("#deadbe")); // Cancel preserves the previous color
    QApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    pick->click();
    picker = controls.findChild<QColorDialog*>("screenDrawingColorPicker");
    CHECK(picker && picker->isVisible());
    picker->setCurrentColor(QColor("#123456"));
    picker->accept();
    CHECK(drawing.penColor() == QColor("#123456"));
    CHECK(editor->text() == "#123456");
    controls.findChild<QPushButton*>("drawingSwatch0")->click();
    CHECK(drawing.penColor() == drawing.chatColor()); // chat color is still available
    CHECK(editor->isHidden());
}

void testUndoFromPresenterToolbarAndTextEditing() {
    QWidget window;
    ScreenDrawing drawing;
    ScreenDrawingSession host("alice", "alice", &drawing, true);
    PresenterOverlay overlay(ShareSource{}, &drawing, &window);
    auto* tools = overlay.findChild<QWidget*>("screenDrawingToolbar");
    auto* controls = tools->findChild<DrawingControls*>();
    drawing.permissionRequested(true);
    drawing.setPenActive(true);
    DrawingCanvas canvas;
    canvas.resize(400, 200);
    canvas.setDrawing(&drawing);
    click(canvas, {100, 100});
    QKeyEvent undo(QEvent::KeyPress, Qt::Key_Z, Qt::ControlModifier);
    QApplication::sendEvent(tools, &undo);
    CHECK(drawing.segments().isEmpty());
    click(canvas, {200, 100});
    controls->findChild<QPushButton*>("drawingSwatch7")->click();
    auto* editor = controls->findChild<QLineEdit*>("drawingHexColor");
    tools->show();
    tools->activateWindow();
    editor->setFocus();
    QApplication::processEvents();
    CHECK(editor->hasFocus());
    editor->selectAll();
    // Keep this edit incomplete: a valid color refreshes the source-less
    // overlay and hides its toolbar, ending text editing on some platforms.
    editor->insert("#12345");
    CHECK(editor->isUndoAvailable());
    QApplication::sendEvent(editor, &undo);
    CHECK(drawing.segments().size() == 1); // the editor's undo does not erase ink
    CHECK(editor->text() != "#12345");
    drawing.setTool(ScreenDrawing::Tool::Select);
    drawing.select({screenStrokeKey(drawing.segments().last())});
    editor->setText("#123456");
    editor->setCursorPosition(7);
    QKeyEvent backspace(QEvent::KeyPress, Qt::Key_Backspace, Qt::NoModifier);
    QApplication::sendEvent(editor, &backspace);
    CHECK(editor->text() == "#12345" && drawing.segments().size() == 1);
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

void testDrawingBans() {
    ScreenDrawing owner, viewer;
    ScreenDrawingSession host("alice", "alice", &owner, true);
    ScreenDrawingSession guest("bob", "alice", &viewer, false);
    QObject::connect(&host, &ScreenDrawingSession::sendToPeer, &guest,
                     [&](const QString& peer, const chat::Message& message) {
                         CHECK(peer == "bob");
                         guest.receive("alice", message);
                     });
    QObject::connect(&guest, &ScreenDrawingSession::sendToPeer, &host,
                     [&](const QString&, const chat::Message& message) { host.receive("bob", message); });
    guest.start();
    owner.permissionRequested(true);
    viewer.setPenActive(true);
    chat::ScreenDraw pen;
    pen.id = 1; pen.stroke = 1;
    viewer.segmentDrawn(pen);
    CHECK(owner.segments().size() == 1);
    // An optimistic local segment that has not reached the presenter is
    // discarded by the canonical snapshot sent alongside the ban.
    pen.id = 2; pen.stroke = 2; pen.author = "bob"; pen.epoch = viewer.epoch();
    viewer.add(pen);
    CHECK(viewer.segments().size() == 2);
    host.setBanned("bob", true);
    CHECK(host.isBanned("bob") && viewer.banned());
    CHECK(viewer.enabled() && !viewer.drawingAllowed() && !viewer.penActive());
    CHECK(viewer.segments().size() == 1); // watching and confirmed ink remain
    DrawingControls controls(&viewer, false);
    bool notice = false;
    for (const auto* label : controls.findChildren<QLabel*>())
        notice |= label->text().contains("banned from drawing");
    CHECK(notice);
    for (const auto* button : controls.findChildren<QPushButton*>()) {
        if (button->text() == "pen" || button->text() == "eraser" || button->text() == "laser")
            CHECK(button->isHidden());
        if (button->text() == "clear mine")
            CHECK(!button->isEnabled());
    }
    viewer.setPenActive(true);
    CHECK(!viewer.penActive());
    viewer.segmentDrawn(pen);
    host.receive("bob", chat::screenDrawMessage(pen, false)); // bypass the controls
    viewer.clearMineRequested();
    host.receive("bob", {chat::MsgType::ScreenDrawErase,
                         {"alice", std::to_string(owner.epoch()), "bob", "1"}});
    CHECK(owner.segments().size() == 1 && viewer.segments().size() == 1);
    pen.id = 3; pen.stroke = 3;
    owner.segmentDrawn(pen);
    CHECK(viewer.segments().size() == 2); // banned people still see everyone else's ink
    guest.setBanned("alice", true);
    host.setBanned("alice", true); // cannot ban the presenter
    CHECK(!guest.isBanned("alice") && !host.isBanned("alice"));
    guest.receive("mallory", {chat::MsgType::ScreenDrawState,
                              {"alice", "1", std::to_string(owner.epoch()), "1"}});
    CHECK(viewer.banned());
    guest.receive("alice", {chat::MsgType::ScreenDrawState,
                            {"alice", "1", std::to_string(owner.epoch()), "bad"}});
    CHECK(viewer.banned());
    guest.stop(); guest.start();
    CHECK(viewer.banned() && viewer.segments().size() == 2);
    host.peerChanged("bob", false);
    guest.peerChanged("alice", true);
    CHECK(viewer.banned());
    owner.clearRequested();
    CHECK(viewer.banned() && viewer.enabled() && viewer.segments().isEmpty());
    owner.permissionRequested(false);
    owner.permissionRequested(true);
    CHECK(viewer.banned());
    host.setBanned("bob", false);
    CHECK(!viewer.banned() && viewer.drawingAllowed());
    pen.id = 4; pen.stroke = 4;
    viewer.segmentDrawn(pen);
    CHECK(owner.segments().size() == 1 && owner.segments()[0].author == "bob");
    host.receive("bob", {chat::MsgType::ScreenDrawState,
                         {"alice", "1", std::to_string(owner.epoch()), "0"}});
    CHECK(!owner.banned()); // viewers cannot moderate the presenter
}

void testAttributionAndPalette() {
    ScreenDrawing drawing;
    ScreenDrawingSession session("alice", "alice", &drawing, true);
    drawing.permissionRequested(true);
    DrawingControls controls(&drawing, true);
    auto* chatSwatch = controls.findChild<QPushButton*>("drawingSwatch0");
    CHECK(chatSwatch && chatSwatch->isChecked() && chatSwatch->accessibleName() == "chat color");
    drawing.setChatColor(QColor("#abcdef"));
    CHECK(drawing.penColor() == QColor("#abcdef"));
    controls.findChild<QPushButton*>("drawingSwatch2")->click(); // yellow, after chat color and red
    CHECK(!chatSwatch->isChecked());
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

void waitForAnimation(int milliseconds) {
    QEventLoop loop;
    QTimer::singleShot(milliseconds, &loop, &QEventLoop::quit);
    loop.exec();
}

void testLaserHoverAndFade() {
    ScreenDrawing drawing;
    ScreenDrawingSession host("alice", "alice", &drawing, true);
    StreamView view;
    view.setDrawing(&drawing);
    view.resize(400, 400);
    QImage image(400, 200, QImage::Format_RGB32);
    image.fill(Qt::black);
    view.setImage(image);
    DrawingControls controls(&drawing, true);
    QPushButton* laser = nullptr;
    for (auto* button : controls.findChildren<QPushButton*>()) {
        if (button->text() == "laser") laser = button;
    }
    CHECK(laser && laser->isHidden());
    drawing.permissionRequested(true);
    drawing.setColor(0);
    laser->click();
    CHECK(laser->isChecked() && drawing.tool() == ScreenDrawing::Tool::Laser);
    CHECK(view.hasMouseTracking());
    mouse(view, QEvent::MouseMove, {100, 50}, Qt::NoButton, Qt::NoButton);
    CHECK(drawing.lasers().isEmpty()); // letterboxing is outside the share
    mouse(view, QEvent::MouseMove, {100, 200}, Qt::NoButton, Qt::NoButton);
    CHECK(drawing.lasers().size() == 1);
    CHECK(drawing.lasers()["alice"].point.x == 2500 && drawing.lasers()["alice"].point.y == 5000);
    waitForAnimation(45);
    mouse(view, QEvent::MouseMove, {300, 200}, Qt::NoButton, Qt::NoButton);
    CHECK(drawing.lasers()["alice"].trail.size() == 2);
    CHECK(drawing.segments().isEmpty() && drawing.historySize() == 0);
    const auto pixel = [](QWidget& widget, int x, int y) {
        const QImage rendered = widget.grab().toImage();
        const auto scale = rendered.devicePixelRatio();
        return rendered.pixelColor(qRound(x * scale), qRound(y * scale));
    };
    CHECK(pixel(view, 200, 200).red() > 100); // the trail connects the hover positions
    view.resize(800, 400);
    CHECK(pixel(view, 600, 200).red() > 100); // pointer remains in normalized video coordinates
    waitForAnimation(ScreenDrawing::laserTrailMs + 100);
    CHECK(drawing.lasers()["alice"].trail.size() <= 1);
    CHECK(drawing.lasers()["alice"].point.visible); // heartbeat keeps a still cursor visible
    CHECK(pixel(view, 400, 200) == QColor(Qt::black)); // old trail has faded
    QEvent leave(QEvent::Leave);
    QApplication::sendEvent(&view, &leave);
    CHECK(!drawing.lasers()["alice"].point.visible);
    waitForAnimation(ScreenDrawing::laserTrailMs + 100);
    CHECK(drawing.lasers().isEmpty());
    CHECK(pixel(view, 600, 200) == QColor(Qt::black));
    mouse(view, QEvent::MouseMove, {400, 200}, Qt::NoButton, Qt::NoButton);
    CHECK(drawing.lasers()["alice"].point.visible);
    QKeyEvent escape(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
    QApplication::sendEvent(&view, &escape);
    CHECK(!drawing.penActive() && !laser->isChecked());
    CHECK(!drawing.lasers()["alice"].point.visible);
    CHECK(drawing.segments().isEmpty());
}

void testLaserTrailHasNoBrightJoints() {
    ScreenDrawing drawing;
    drawing.setState(true, 1);
    DrawingCanvas canvas;
    canvas.resize(400, 200);
    canvas.setDrawing(&drawing);
    chat::ScreenLaser point{"alice", "bob", 1, 1, 2500, 5000, 0xff5470, true};
    drawing.addLaser(point);
    point.id = 2; point.x = 5000;
    drawing.addLaser(point);
    point.id = 3; point.x = 7500;
    drawing.addLaser(point);
    // Partially faded segments expose doubled alpha where round caps overlap.
    waitForAnimation(300);
    QImage image(canvas.size(), QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);
    QPainter painter(&image);
    canvas.renderDrawing(painter);
    painter.end();
    const int midpoint = image.pixelColor(150, 100).alpha();
    const int joint = image.pixelColor(200, 100).alpha();
    CHECK(midpoint > 50 && midpoint < 200);
    CHECK(qAbs(joint - midpoint) < 10);
}

void testLaserPeerPermissionsAndExpiry() {
    ScreenDrawing own, viewer, late;
    ScreenDrawingSession host("alice", "alice", &own, true);
    ScreenDrawingSession guest("bob", "alice", &viewer, false);
    ScreenDrawingSession lateGuest("carol", "alice", &late, false);
    QObject::connect(&host, &ScreenDrawingSession::sendToPeer, &host,
                     [&](const QString& peer, const chat::Message& message) {
                         if (peer == "bob") guest.receive("alice", message);
                         else if (peer == "carol") lateGuest.receive("alice", message);
                     });
    QObject::connect(&guest, &ScreenDrawingSession::sendToPeer, &host,
                     [&](const QString&, const chat::Message& message) { host.receive("bob", message); });
    QObject::connect(&lateGuest, &ScreenDrawingSession::sendToPeer, &host,
                     [&](const QString&, const chat::Message& message) { host.receive("carol", message); });
    guest.start();
    chat::ScreenLaser point{"wrong", "spoofed", 0, 1, 5000, 5000, 0x12ab34, true};
    viewer.laserPointed(point);
    CHECK(own.lasers().isEmpty()); // drawing is off
    own.permissionRequested(true);
    chat::ScreenDraw ink;
    ink.id = 500;
    own.segmentDrawn(ink);
    viewer.setTool(ScreenDrawing::Tool::Laser);
    viewer.laserPointed(point);
    CHECK(own.lasers().size() == 1 && viewer.lasers().size() == 1);
    CHECK(own.lasers()["bob"].point.author == "bob" && own.lasers()["bob"].point.sharer == "alice");
    CHECK(viewer.lasers()["bob"].trail.size() == 1); // owner's echo is deduplicated
    lateGuest.start();
    CHECK(late.enabled() && late.segments().size() == 1 && late.lasers().isEmpty()); // only ink is replayed
    ++point.id;
    viewer.laserPointed(point);
    CHECK(late.lasers().contains("bob"));
    point.sharer = "alice"; point.epoch = own.epoch(); point.id = 100;
    host.receive("outsider", chat::screenLaserMessage(point, false));
    guest.receive("outsider", chat::screenLaserMessage(point, true));
    CHECK(own.lasers().size() == 1 && viewer.lasers().size() == 1);
    host.setBanned("bob", true);
    CHECK(!viewer.penActive());
    const auto before = own.lasers()["bob"].point.id;
    viewer.laserPointed(point);
    host.receive("bob", chat::screenLaserMessage(point, false));
    CHECK(own.lasers()["bob"].point.id == before); // even a client bypassing its UI is rejected
    CHECK(!viewer.lasers().contains("spoofed"));
    host.setBanned("bob", false);
    host.receive("bob", chat::screenLaserMessage(point, false));
    CHECK(own.lasers()["bob"].point.id == 100);
    --point.id;
    point.x = 1000;
    host.receive("bob", chat::screenLaserMessage(point, false));
    CHECK(own.lasers()["bob"].point.x == 5000); // delayed positions cannot move it backwards
    point.id = 101; point.author = "alice";
    own.laserPointed(point);
    CHECK(viewer.lasers().contains("alice") && late.lasers().contains("alice"));
    CHECK(own.segments().size() == 1 && viewer.historySize() == 1);
    waitForAnimation(ScreenDrawing::laserTimeoutMs + 100);
    CHECK(own.lasers().isEmpty() && viewer.lasers().isEmpty() && late.lasers().isEmpty());
    CHECK(own.segments().size() == 1 && viewer.segments().size() == 1); // fading leaves ink alone
    own.laserPointed(point);
    CHECK(own.lasers().isEmpty()); // expiry retains sequence deduplication
    ++point.id;
    own.laserPointed(point);
    CHECK(!viewer.lasers().isEmpty());
    own.permissionRequested(false);
    CHECK(own.lasers().isEmpty() && viewer.lasers().isEmpty() && late.lasers().isEmpty());
    host.receive("bob", chat::screenLaserMessage(point, false));
    CHECK(own.lasers().isEmpty());

    ScreenDrawing bounded;
    bounded.setState(true, 1);
    point.epoch = 1;
    for (quint32 id = 1; id <= 200; ++id) {
        point.id = id;
        point.x = static_cast<int>(id);
        CHECK(bounded.addLaser(point));
    }
    CHECK(bounded.lasers()["alice"].trail.size() == 64);
    CHECK(bounded.historySize() == 0 && bounded.segments().isEmpty());
    ++point.id;
    point.epoch = 2;
    CHECK(!bounded.addLaser(point)); // delayed positions from another clear/share
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
    testUndoOwnStrokes();
    testHexColorsAndPicker();
    testUndoFromPresenterToolbarAndTextEditing();
    testEraserAndSelection();
    testDrawingBans();
    testAttributionAndPalette();
    testLaserHoverAndFade();
    testLaserTrailHasNoBrightJoints();
    testLaserPeerPermissionsAndExpiry();
    testDrawingHistoryLimitAndMissingPeer();
    std::puts("screen_drawing_test: ok");
}
