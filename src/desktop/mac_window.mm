#include "desktop/mac_window.h"

#include <QGuiApplication>
#include <QWidget>

#import <AppKit/AppKit.h>

void hideNativeWindowTitle(QWidget* window) {
    // Only Cocoa windows are backed by an NSView; offscreen ones are not.
    if (QGuiApplication::platformName() != "cocoa")
        return;
    auto* view = (__bridge NSView*)reinterpret_cast<void*>(window->winId());
    view.window.titleVisibility = NSWindowTitleHidden;
}
