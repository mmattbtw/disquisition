#include <QWidget>
#import <AppKit/AppKit.h>

void configureMacPenguinOverlay(QWidget* widget) {
    NSView* view = reinterpret_cast<NSView*>(widget->winId());
    NSWindow* window = view.window;
    window.level = NSStatusWindowLevel;
    window.ignoresMouseEvents = YES;
    window.hidesOnDeactivate = NO;
    window.hasShadow = NO;
    window.collectionBehavior = NSWindowCollectionBehaviorCanJoinAllSpaces |
                                NSWindowCollectionBehaviorFullScreenAuxiliary |
                                NSWindowCollectionBehaviorStationary |
                                NSWindowCollectionBehaviorIgnoresCycle;
}

bool macPenguinReducedMotion() {
    return NSWorkspace.sharedWorkspace.accessibilityDisplayShouldReduceMotion;
}
