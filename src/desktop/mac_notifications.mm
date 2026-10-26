#include "desktop/mac_notifications.h"

#import <AppKit/AppKit.h>
#import <UserNotifications/UserNotifications.h>

#include <utility>

@interface DisquisitionNotificationDelegate : NSObject <UNUserNotificationCenterDelegate> {
@public
    std::function<void()> activatedCallback;
}
@end

@implementation DisquisitionNotificationDelegate

// Without this, notifications posted while the app is frontmost (window
// minimized or behind a dialog) are dropped.
- (void)userNotificationCenter:(UNUserNotificationCenter*)center
       willPresentNotification:(UNNotification*)notification
         withCompletionHandler:(void (^)(UNNotificationPresentationOptions))completionHandler {
    completionHandler(UNNotificationPresentationOptionBanner | UNNotificationPresentationOptionList |
                      UNNotificationPresentationOptionSound);
}

- (void)userNotificationCenter:(UNUserNotificationCenter*)center
    didReceiveNotificationResponse:(UNNotificationResponse*)response
             withCompletionHandler:(void (^)(void))completionHandler {
    if ([response.actionIdentifier isEqualToString:UNNotificationDefaultActionIdentifier]) {
        // ~MacNotifications clears the callback on the main thread, so it
        // is checked there too.
        dispatch_async(dispatch_get_main_queue(), ^{
          if (activatedCallback) activatedCallback();
        });
    }
    completionHandler();
}

@end

struct MacNotifications::State {
    DisquisitionNotificationDelegate* delegate = nil;
};

MacNotifications::MacNotifications(std::function<void()> activated) : state_(new State) {
    // UNUserNotificationCenter throws outside an app bundle.
    if (NSBundle.mainBundle.bundleIdentifier == nil) return;
    state_->delegate = [[DisquisitionNotificationDelegate alloc] init];
    state_->delegate->activatedCallback = std::move(activated);
    UNUserNotificationCenter* center = UNUserNotificationCenter.currentNotificationCenter;
    center.delegate = state_->delegate;
    const UNAuthorizationOptions options =
        UNAuthorizationOptionAlert | UNAuthorizationOptionSound | UNAuthorizationOptionBadge;
    [center requestAuthorizationWithOptions:options completionHandler:^(BOOL, NSError*) {}];
}

MacNotifications::~MacNotifications() {
    if (state_->delegate) {
        state_->delegate->activatedCallback = nullptr;
        UNUserNotificationCenter* center = UNUserNotificationCenter.currentNotificationCenter;
        if (center.delegate == state_->delegate) center.delegate = nil;
    }
    delete state_;
}

void MacNotifications::show(const QString& key, const QString& title, const QString& body) {
    if (!state_->delegate) return;
    UNMutableNotificationContent* content = [[UNMutableNotificationContent alloc] init];
    content.title = title.toNSString();
    content.body = body.toNSString();
    content.sound = UNNotificationSound.defaultSound;
    content.threadIdentifier = @"room";
    UNNotificationRequest* request = [UNNotificationRequest requestWithIdentifier:key.toNSString()
                                                                          content:content
                                                                          trigger:nil];
    [UNUserNotificationCenter.currentNotificationCenter addNotificationRequest:request
                                                         withCompletionHandler:nil];
}

void MacNotifications::removeAll() {
    if (!state_->delegate) return;
    UNUserNotificationCenter* center = UNUserNotificationCenter.currentNotificationCenter;
    [center removeAllPendingNotificationRequests];
    [center removeAllDeliveredNotifications];
}

void MacNotifications::setBadgeLabel(const QString& label) {
    NSApp.dockTile.badgeLabel = label.isEmpty() ? nil : label.toNSString();
}
