#include "desktop/mac_screen_capture.h"

#import <CoreGraphics/CoreGraphics.h>
#import <CoreMedia/CoreMedia.h>
#import <CoreVideo/CoreVideo.h>
#import <ScreenCaptureKit/ScreenCaptureKit.h>

#include <atomic>
#include <utility>

@interface DisquisitionScreenOutput : NSObject <SCStreamOutput, SCStreamDelegate> {
@public
    std::atomic_bool active;
    std::function<void(QImage)> frameCallback;
    std::function<void(QString)> errorCallback;
    SCStream* stream;
}
@end

@implementation DisquisitionScreenOutput

- (void)stream:(SCStream*)source didOutputSampleBuffer:(CMSampleBufferRef)sample
        ofType:(SCStreamOutputType)type {
    if (!active || type != SCStreamOutputTypeScreen || !CMSampleBufferIsValid(sample)) return;
    CFArrayRef attachments = CMSampleBufferGetSampleAttachmentsArray(sample, false);
    if (attachments && CFArrayGetCount(attachments) > 0) {
        NSDictionary* info = (__bridge NSDictionary*)CFArrayGetValueAtIndex(attachments, 0);
        NSNumber* status = info[SCStreamFrameInfoStatus];
        if (status && status.integerValue != SCFrameStatusComplete) return;
    }
    CVPixelBufferRef pixels = CMSampleBufferGetImageBuffer(sample);
    if (!pixels || CVPixelBufferGetPixelFormatType(pixels) != kCVPixelFormatType_32BGRA ||
        CVPixelBufferLockBaseAddress(pixels, kCVPixelBufferLock_ReadOnly) != kCVReturnSuccess) return;
    QImage image(static_cast<const uchar*>(CVPixelBufferGetBaseAddress(pixels)),
                 static_cast<int>(CVPixelBufferGetWidth(pixels)),
                 static_cast<int>(CVPixelBufferGetHeight(pixels)),
                 static_cast<qsizetype>(CVPixelBufferGetBytesPerRow(pixels)), QImage::Format_ARGB32);
    QImage copy = image.copy();
    CVPixelBufferUnlockBaseAddress(pixels, kCVPixelBufferLock_ReadOnly);
    if (active && !copy.isNull()) frameCallback(std::move(copy));
}

- (void)stream:(SCStream*)source didStopWithError:(NSError*)error {
    if (active.exchange(false)) errorCallback(QString::fromUtf8(error.localizedDescription.UTF8String));
}

@end

namespace {

QString toQString(NSString* text) {
    return QString::fromUtf8(text.UTF8String);
}

// Picks what to capture from the shareable content and sets the frame size
// in points, or returns nil when it is no longer there.
using FilterChooser = SCContentFilter* (^)(SCShareableContent* content, CGSize* size);

void startStream(DisquisitionScreenOutput* output, NSString* missing, FilterChooser choose) {
    output->active = true;
    [SCShareableContent getShareableContentExcludingDesktopWindows:NO
                                       onScreenWindowsOnly:NO
                                         completionHandler:^(SCShareableContent* content, NSError* error) {
      dispatch_async(dispatch_get_main_queue(), ^{
        if (!output->active) return;
        if (error) {
            output->errorCallback(toQString(error.localizedDescription));
            return;
        }
        CGSize size = CGSizeZero;
        SCContentFilter* filter = choose(content, &size);
        if (!filter || size.width < 2 || size.height < 2) {
            output->errorCallback(toQString(missing));
            return;
        }
        SCStreamConfiguration* configuration = [[SCStreamConfiguration alloc] init];
        configuration.width = static_cast<size_t>(size.width);
        configuration.height = static_cast<size_t>(size.height);
        configuration.pixelFormat = kCVPixelFormatType_32BGRA;
        configuration.minimumFrameInterval = CMTimeMake(1, 30);
        configuration.queueDepth = 3;
        configuration.showsCursor = YES;
        output->stream = [[SCStream alloc] initWithFilter:filter configuration:configuration delegate:output];
        NSError* addError = nil;
        if (![output->stream addStreamOutput:output type:SCStreamOutputTypeScreen
                          sampleHandlerQueue:dispatch_get_global_queue(QOS_CLASS_USER_INTERACTIVE, 0)
                                      error:&addError]) {
            output->errorCallback(toQString(addError.localizedDescription));
            return;
        }
        [output->stream startCaptureWithCompletionHandler:^(NSError* startError) {
            if (output->active && startError)
                output->errorCallback(toQString(startError.localizedDescription));
        }];
      });
    }];
}

QList<ShareSource> windowSources(SCShareableContent* content) {
    const pid_t self = NSProcessInfo.processInfo.processIdentifier;
    QList<ShareSource> sources;
    for (SCWindow* window in content.windows) {
        // Layer 0 holds ordinary windows, not menus, the Dock or overlays.
        if (window.windowLayer != 0 || window.owningApplication.processID == self ||
            window.frame.size.width < 64 || window.frame.size.height < 64) {
            continue;
        }
        const QString application = toQString(window.owningApplication.applicationName);
        const QString title = toQString(window.title);
        if (title.isEmpty() && application.isEmpty()) continue;
        ShareSource source;
        source.kind = ShareSource::Kind::Window;
        source.title = title.isEmpty() ? application : title;
        source.detail = title.isEmpty() ? QString() : application;
        source.nativeId = window.windowID;
        sources.append(source);
    }
    return sources;
}

QList<ShareSource> displaySources(SCShareableContent* content) {
    QList<ShareSource> sources;
    int number = 1;
    for (SCDisplay* display in content.displays) {
        ShareSource source;
        source.kind = ShareSource::Kind::Screen;
        source.title = QString("Screen %1").arg(number++) +
                       (display.displayID == CGMainDisplayID() ? " (main)" : "");
        source.detail = QString("%1x%2").arg(display.width).arg(display.height);
        source.nativeId = display.displayID;
        sources.append(source);
    }
    return sources;
}

} // namespace

struct MacScreenCapture::State {
    DisquisitionScreenOutput* output;
};

MacScreenCapture::MacScreenCapture(std::function<void(QImage)> frame,
                                   std::function<void(QString)> error)
    : state_(new State {[[DisquisitionScreenOutput alloc] init]}) {
    state_->output->active = false;
    state_->output->frameCallback = std::move(frame);
    state_->output->errorCallback = std::move(error);
}

MacScreenCapture::~MacScreenCapture() {
    stop();
    delete state_;
}

void MacScreenCapture::startDisplay(quint32 displayId) {
    startStream(state_->output, @"the screen is no longer available",
                ^SCContentFilter*(SCShareableContent* content, CGSize* size) {
      for (SCDisplay* display in content.displays) {
          if (display.displayID == displayId) {
              *size = CGSizeMake(display.width, display.height);
              return [[SCContentFilter alloc] initWithDisplay:display
                                        excludingApplications:@[]
                                             exceptingWindows:@[]];
          }
      }
      return nil;
    });
}

void MacScreenCapture::startWindow(quint32 windowId) {
    startStream(state_->output, @"the window is no longer available",
                ^SCContentFilter*(SCShareableContent* content, CGSize* size) {
      for (SCWindow* window in content.windows) {
          if (window.windowID == windowId) {
              *size = window.frame.size;
              return [[SCContentFilter alloc] initWithDesktopIndependentWindow:window];
          }
      }
      return nil;
    });
}

void MacScreenCapture::stop() {
    DisquisitionScreenOutput* output = state_->output;
    output->active = false;
    if (output->stream) {
        [output->stream stopCaptureWithCompletionHandler:nil];
        output->stream = nil;
    }
}

bool MacScreenCapture::running() const { return state_->output->active; }

void listMacShareSources(std::function<void(QList<ShareSource>)> done) {
    [SCShareableContent getShareableContentExcludingDesktopWindows:YES
                                       onScreenWindowsOnly:YES
                                         completionHandler:^(SCShareableContent* content, NSError* error) {
      done(error ? QList<ShareSource>() : windowSources(content) + displaySources(content));
    }];
}
