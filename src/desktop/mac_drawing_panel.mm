#include "desktop/mac_drawing_panel.h"
#include "desktop/screen_drawing.h"

#include <QApplication>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QImage>
#include <QPainter>
#include <QWidget>
#include <QWindow>
#import <AppKit/AppKit.h>

@interface DisquisitionDrawingPanel : NSPanel
@property(nonatomic, assign) QWidget* target;
@end

@implementation DisquisitionDrawingPanel
- (BOOL)canBecomeKeyWindow { return YES; }
- (BOOL)canBecomeMainWindow { return NO; }
- (void)sendEvent:(NSEvent*)event {
    // AppKit raises an exception if keyCode is read from a mouse event.
    if (event.type == NSEventTypeKeyDown && self.target) {
        const int key = event.keyCode == 53 ? Qt::Key_Escape :
                        event.keyCode == 51 ? Qt::Key_Backspace :
                        event.keyCode == 117 ? Qt::Key_Delete : 0;
        if (key) {
            QKeyEvent keyEvent(QEvent::KeyPress, key, Qt::NoModifier);
            keyEvent.setAccepted(false);
            QApplication::sendEvent(self.target, &keyEvent);
            if (keyEvent.isAccepted())
                return;
        }
    }
    [super sendEvent:event];
}
@end

// A native canvas receives input while another app remains active. Forward
// pointer events to the same Qt canvas used by video previews and fullscreen.
@interface DisquisitionDrawingView : NSView {
@public
    QPointer<ScreenDrawing> drawingModel;
    QImage drawingImage;
}
@property(nonatomic, assign) QWidget* target;
@end

@implementation DisquisitionDrawingView
- (BOOL)isFlipped { return YES; }
- (BOOL)acceptsFirstResponder { return YES; }
- (BOOL)acceptsFirstMouse:(NSEvent*)event { Q_UNUSED(event); return YES; }
- (void)resetCursorRects { [self addCursorRect:self.bounds cursor:NSCursor.crosshairCursor]; }
- (void)forwardMouse:(NSEvent*)event type:(QEvent::Type)type {
    NSPoint point = [self convertPoint:event.locationInWindow fromView:nil];
    const QPointF position(point.x, point.y);
    const auto button = type == QEvent::MouseMove ? Qt::NoButton : Qt::LeftButton;
    const auto buttons = type == QEvent::MouseButtonRelease ? Qt::NoButton : Qt::LeftButton;
    QMouseEvent mouse(type, position, position, button, buttons, Qt::NoModifier);
    QApplication::sendEvent(self.target, &mouse);
}
- (void)mouseDown:(NSEvent*)event { [self forwardMouse:event type:QEvent::MouseButtonPress]; }
- (void)mouseDragged:(NSEvent*)event { [self forwardMouse:event type:QEvent::MouseMove]; }
- (void)mouseUp:(NSEvent*)event { [self forwardMouse:event type:QEvent::MouseButtonRelease]; }
- (void)drawRect:(NSRect)dirtyRect {
    Q_UNUSED(dirtyRect);
    if (!drawingModel || !drawingModel->enabled())
        return;
    // Reuse the Qt renderer so native presenter panels have exactly the same
    // selection highlights, colors and author labels as viewers' video canvases.
    const qreal scale = self.window.backingScaleFactor;
    const QSize size(qRound(self.bounds.size.width * scale), qRound(self.bounds.size.height * scale));
    if (drawingImage.size() != size) {
        drawingImage = QImage(size, QImage::Format_ARGB32_Premultiplied);
    }
    drawingImage.setDevicePixelRatio(scale);
    drawingImage.fill(Qt::transparent);
    QPainter painter(&drawingImage);
    static_cast<DrawingCanvas*>(self.target)->renderDrawing(painter);
    painter.end();
    CGColorSpaceRef colors = CGColorSpaceCreateDeviceRGB();
    CGDataProviderRef data = CGDataProviderCreateWithData(nullptr, drawingImage.constBits(),
                                                        drawingImage.sizeInBytes(), nullptr);
    CGImageRef image = CGImageCreate(size.width(), size.height(), 8, 32, drawingImage.bytesPerLine(),
        colors, static_cast<CGBitmapInfo>(kCGImageAlphaPremultipliedFirst) | kCGBitmapByteOrder32Little, data, nullptr, false,
        kCGRenderingIntentDefault);
    CGContextRef context = NSGraphicsContext.currentContext.CGContext;
    CGContextSaveGState(context);
    CGContextClearRect(context, NSRectToCGRect(self.bounds));
    CGContextTranslateCTM(context, 0, self.bounds.size.height);
    CGContextScaleCTM(context, 1, -1);
    if (image)
        CGContextDrawImage(context, NSRectToCGRect(self.bounds), image);
    CGContextRestoreGState(context);
    if (image) CGImageRelease(image);
    CGDataProviderRelease(data);
    CGColorSpaceRelease(colors);
}

@end

struct MacDrawingPanel::Native {
    DisquisitionDrawingPanel* panel;
    std::unique_ptr<QWindow> host;
    DisquisitionDrawingView* canvas = nil;
};

MacDrawingPanel::MacDrawingPanel(QWidget* widget, ScreenDrawing* drawing) :
    QObject(widget), native_(std::make_unique<Native>()), widget_(widget) {
    // AppKit needs the nonactivating style at construction time. Changing the
    // style of a Qt-created panel later does not update its activation behavior.
    native_->panel = [[DisquisitionDrawingPanel alloc]
        initWithContentRect:NSMakeRect(0, 0, 1, 1)
        styleMask:NSWindowStyleMaskBorderless | NSWindowStyleMaskNonactivatingPanel
        backing:NSBackingStoreBuffered defer:NO];
    DisquisitionDrawingPanel* panel = native_->panel;
    panel.target = widget_;
    panel.title = widget_->windowTitle().toNSString();
    panel.releasedWhenClosed = NO;
    panel.hidesOnDeactivate = NO;
    // Keep the toolbar clickable after a pen stroke makes the canvas key.
    panel.level = NSModalPanelWindowLevel + (drawing ? 0 : 1);
    panel.opaque = NO;
    panel.backgroundColor = NSColor.clearColor;
    panel.hasShadow = NO;
    panel.collectionBehavior = NSWindowCollectionBehaviorFullScreenAuxiliary |
                               NSWindowCollectionBehaviorMoveToActiveSpace;
    if (drawing) {
        // The Qt canvas supplies shared input handling but has no visible native
        // window. Give its backing window a distinct title for accessibility.
        widget_->setWindowTitle("Screen drawing backing canvas");
        native_->canvas = [[DisquisitionDrawingView alloc] initWithFrame:panel.contentView.bounds];
        native_->canvas.accessibilityElement = YES;
        native_->canvas.accessibilityRole = NSAccessibilityGroupRole;
        native_->canvas.accessibilityLabel = @"Shared screen drawing canvas";
        native_->canvas.target = widget_;
        native_->canvas->drawingModel = drawing;
        panel.contentView = native_->canvas;
    } else {
        native_->host.reset(QWindow::fromWinId(reinterpret_cast<WId>(panel.contentView)));
        widget_->winId();
        widget_->windowHandle()->setParent(native_->host.get());
    }
}

MacDrawingPanel::~MacDrawingPanel() {
    // QObject deletes this host after QWidget has destroyed its embedded view.
    native_->panel.target = nullptr;
    [native_->panel close];
}

void MacDrawingPanel::show(const QRect& bounds, bool transparent) {
    // Capture and Qt use top-left desktop coordinates; AppKit uses bottom-left.
    const CGFloat desktopTop = NSScreen.screens.firstObject.frame.size.height;
    NSPanel* panel = native_->panel;
    const NSRect frame = NSMakeRect(bounds.x(), desktopTop - bounds.y() - bounds.height(),
                                    bounds.width(), bounds.height());
    if (!NSEqualRects(panel.frame, frame))
        [panel setFrame:frame display:YES];
    panel.ignoresMouseEvents = transparent;
    widget_->setGeometry(0, 0, bounds.width(), bounds.height());
    if (!native_->canvas && !widget_->isVisible())
        widget_->show();
    if (!panel.visible)
        [panel orderFront:nil];
    if (native_->canvas && transparent && panel.keyWindow) {
        [panel resignKeyWindow];
    }
}

void MacDrawingPanel::hide() {
    widget_->hide();
    [native_->panel orderOut:nil];
}

void MacDrawingPanel::update() {
    native_->canvas.needsDisplay = YES;
}
