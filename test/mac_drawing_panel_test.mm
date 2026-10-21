#include <QApplication>
#include <CoreGraphics/CoreGraphics.h>
#import <AppKit/AppKit.h>

#include "check.h"
#include "desktop/screen_drawing.h"

int main(int argc, char** argv) {
    // Avoid Qt activating this test process during AppKit launch completion.
    QCoreApplication::setAttribute(Qt::AA_PluginApplication);
    QApplication app(argc, argv);
    QWidget mainWindow;
    ScreenDrawing drawing;
    ShareSource source;
    source.nativeId = CGMainDisplayID();
    PresenterOverlay overlay(source, &drawing, &mainWindow);
    QObject::connect(&drawing, &ScreenDrawing::segmentDrawn, &drawing,
                     [&](const chat::ScreenDraw& segment) { drawing.add(segment); });
    const bool wasActive = NSApp.active;
    CHECK(!wasActive);
    drawing.setState(true, 1);
    NSPanel* canvas = nil;
    NSPanel* toolbar = nil;
    for (NSWindow* window in NSApp.windows) {
        if (!window.visible)
            continue;
        if ([window.title isEqualToString:@"Screen drawing overlay"])
            canvas = static_cast<NSPanel*>(window);
        if ([window.title isEqualToString:@"Screen drawing"])
            toolbar = static_cast<NSPanel*>(window);
    }
    CHECK(canvas && toolbar);
    CHECK((canvas.styleMask & NSWindowStyleMaskNonactivatingPanel) != 0);
    CHECK((toolbar.styleMask & NSWindowStyleMaskNonactivatingPanel) != 0);
    CHECK(canvas.visible && toolbar.visible && canvas.ignoresMouseEvents);
    CHECK(toolbar.level > canvas.level);
    CHECK(NSApp.active == wasActive);
    CHECK(!mainWindow.isVisible());

    drawing.setPenActive(true);
    CHECK(!canvas.ignoresMouseEvents);
    CHECK(NSApp.active == wasActive);
    CHECK(!mainWindow.isVisible());
    const auto mouse = [&](NSEventType type, CGFloat x, CGFloat y) {
        return [NSEvent mouseEventWithType:type location:NSMakePoint(x, y)
            modifierFlags:0 timestamp:0 windowNumber:canvas.windowNumber context:nil
            eventNumber:0 clickCount:1 pressure:1];
    };
    [canvas.contentView mouseDown:mouse(NSEventTypeLeftMouseDown, 100, 100)];
    [canvas.contentView mouseUp:mouse(NSEventTypeLeftMouseUp, 300, 200)];
    CHECK(drawing.segments().size() == 2);
    CHECK(drawing.segments()[0].x1 < drawing.segments()[1].x2);
    CHECK(drawing.segments()[0].y1 > drawing.segments()[1].y2);
    // Exercise AppKit key dispatch, including when Qt's application shortcut
    // machinery has no active main window.
    NSEvent* escape = [NSEvent keyEventWithType:NSEventTypeKeyDown location:NSZeroPoint
        modifierFlags:0 timestamp:0 windowNumber:canvas.windowNumber context:nil
        characters:@"\x1b" charactersIgnoringModifiers:@"\x1b" isARepeat:NO keyCode:53];
    [canvas sendEvent:escape];
    CHECK(!drawing.penActive() && canvas.ignoresMouseEvents);
    CHECK(NSApp.active == wasActive);
    drawing.setPenActive(true);
    [toolbar sendEvent:escape];
    CHECK(!drawing.penActive() && canvas.ignoresMouseEvents);
    CHECK(NSApp.active == wasActive);
    drawing.setState(false, 2);
    CHECK(!canvas.visible && !toolbar.visible);
    std::puts("mac_drawing_panel_test: ok");
}
