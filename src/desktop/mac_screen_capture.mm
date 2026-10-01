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

void MacScreenCapture::start() {
    DisquisitionScreenOutput* output = state_->output;
    output->active = true;
    [SCShareableContent getShareableContentExcludingDesktopWindows:NO
                                       onScreenWindowsOnly:NO
                                         completionHandler:^(SCShareableContent* content, NSError* error) {
      dispatch_async(dispatch_get_main_queue(), ^{
        if (!output->active) return;
        if (error) {
            output->errorCallback(QString::fromUtf8(error.localizedDescription.UTF8String));
            return;
        }
        SCDisplay* display = nil;
        for (SCDisplay* candidate in content.displays) {
            if (candidate.displayID == CGMainDisplayID()) { display = candidate; break; }
        }
        if (!display) display = content.displays.firstObject;
        if (!display) {
            output->errorCallback(QStringLiteral("no display is available for capture"));
            return;
        }
        SCContentFilter* filter = [[SCContentFilter alloc] initWithDisplay:display
                                                     excludingApplications:@[] exceptingWindows:@[]];
        SCStreamConfiguration* configuration = [[SCStreamConfiguration alloc] init];
        configuration.width = display.width;
        configuration.height = display.height;
        configuration.pixelFormat = kCVPixelFormatType_32BGRA;
        configuration.minimumFrameInterval = CMTimeMake(1, 30);
        configuration.queueDepth = 3;
        output->stream = [[SCStream alloc] initWithFilter:filter configuration:configuration delegate:output];
        NSError* addError = nil;
        if (![output->stream addStreamOutput:output type:SCStreamOutputTypeScreen
                          sampleHandlerQueue:dispatch_get_global_queue(QOS_CLASS_USER_INTERACTIVE, 0)
                                      error:&addError]) {
            output->errorCallback(QString::fromUtf8(addError.localizedDescription.UTF8String));
            return;
        }
        [output->stream startCaptureWithCompletionHandler:^(NSError* startError) {
            if (output->active && startError)
                output->errorCallback(QString::fromUtf8(startError.localizedDescription.UTF8String));
        }];
      });
    }];
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
