#include "desktop/push_to_talk.h"

#include <QGuiApplication>
#include <QKeyEvent>
#include <QKeySequence>

#if defined(Q_OS_WIN)
#    include <qt_windows.h>
#elif defined(Q_OS_MACOS)
#    include <CoreGraphics/CoreGraphics.h>
#    include <IOKit/hid/IOHIDKeys.h>
#    include <IOKit/hid/IOHIDManager.h>
#    include <IOKit/hid/IOHIDUsageTables.h>
#elif defined(DISQUISITION_HAVE_XCB)
#    include <xcb/xcb.h>
#    include <cstdlib>
#    include <memory>
#endif

#if defined(Q_OS_MACOS)
namespace {

// The virtual key code macOS gives Caps Lock, kVK_CapsLock.
constexpr quint32 kCapsLockKey = 0x39;

CFDictionaryRef usageMatching(CFStringRef pageKey, int page, CFStringRef usageKey, int usage) {
    CFNumberRef pageNumber = CFNumberCreate(nullptr, kCFNumberIntType, &page);
    CFNumberRef usageNumber = CFNumberCreate(nullptr, kCFNumberIntType, &usage);
    const void* keys[] = {pageKey, usageKey};
    const void* values[] = {pageNumber, usageNumber};
    CFDictionaryRef matching = CFDictionaryCreate(nullptr, keys, values, 2,
                                                  &kCFTypeDictionaryKeyCallBacks,
                                                  &kCFTypeDictionaryValueCallBacks);
    CFRelease(pageNumber);
    CFRelease(usageNumber);
    return matching;
}

void closeKeyboards(IOHIDManagerRef keyboards) {
    IOHIDManagerUnscheduleFromRunLoop(keyboards, CFRunLoopGetMain(), kCFRunLoopCommonModes);
    IOHIDManagerClose(keyboards, kIOHIDOptionsTypeNone);
    CFRelease(keyboards);
}

}  // namespace
#endif

PushToTalk::PushToTalk(QObject* parent) : QObject(parent) {
    poll_.setInterval(15);
    connect(&poll_, &QTimer::timeout, this, &PushToTalk::poll);
#if defined(DISQUISITION_HAVE_XCB)
    // XWayland only sees keys sent to X clients, so read the keymap only
    // when the app itself runs on X11.
    if (QGuiApplication::platformName() == "xcb") {
        x11_ = xcb_connect(nullptr, nullptr);
        if (xcb_connection_has_error(x11_)) {
            xcb_disconnect(x11_);
            x11_ = nullptr;
        }
    }
#endif
}

PushToTalk::~PushToTalk() {
#if defined(Q_OS_MACOS)
    if (capsLock_) closeKeyboards(capsLock_);
#endif
#if defined(DISQUISITION_HAVE_XCB)
    if (x11_) xcb_disconnect(x11_);
#endif
}

void PushToTalk::start(quint32 key) {
    stop();
    key_ = key;
#if defined(Q_OS_WIN)
    global_ = true;
#elif defined(Q_OS_MACOS)
    // Reading keys while another app is focused needs Input Monitoring.
    // macOS asks once and applies a grant after the app restarts.
    global_ = CGPreflightListenEventAccess();
    if (!global_) CGRequestListenEventAccess();
    // macOS tracks whether Caps Lock is on rather than held down, so read
    // the key from the keyboards themselves.
    if (global_ && key == kCapsLockKey) {
        IOHIDManagerRef keyboards = IOHIDManagerCreate(kCFAllocatorDefault, kIOHIDOptionsTypeNone);
        CFDictionaryRef devices =
            usageMatching(CFSTR(kIOHIDDeviceUsagePageKey), kHIDPage_GenericDesktop,
                          CFSTR(kIOHIDDeviceUsageKey), kHIDUsage_GD_Keyboard);
        CFDictionaryRef capsLock =
            usageMatching(CFSTR(kIOHIDElementUsagePageKey), kHIDPage_KeyboardOrKeypad,
                          CFSTR(kIOHIDElementUsageKey), kHIDUsage_KeyboardCapsLock);
        IOHIDManagerSetDeviceMatching(keyboards, devices);
        IOHIDManagerSetInputValueMatching(keyboards, capsLock);
        CFRelease(devices);
        CFRelease(capsLock);
        IOHIDManagerRegisterInputValueCallback(
            keyboards,
            [](void* context, IOReturn, void*, IOHIDValueRef value) {
                static_cast<PushToTalk*>(context)->setHeld(IOHIDValueGetIntegerValue(value) != 0);
            },
            this);
        IOHIDManagerScheduleWithRunLoop(keyboards, CFRunLoopGetMain(), kCFRunLoopCommonModes);
        if (IOHIDManagerOpen(keyboards, kIOHIDOptionsTypeNone) == kIOReturnSuccess) {
            capsLock_ = keyboards;
            return;
        }
        closeKeyboards(keyboards);
    }
#else
    global_ = x11_ != nullptr;
#endif
    if (global_) {
        poll_.start();
        poll();
    } else {
        qApp->installEventFilter(this);
    }
}

void PushToTalk::stop() {
    key_.reset();
    poll_.stop();
    qApp->removeEventFilter(this);
#if defined(Q_OS_MACOS)
    if (capsLock_) closeKeyboards(capsLock_);
    capsLock_ = nullptr;
#endif
    setHeld(false);
}

quint32 PushToTalk::nativeKey(const QKeyEvent* event) {
#if defined(Q_OS_WIN) || defined(Q_OS_MACOS)
    return event->nativeVirtualKey();
#else
    return event->nativeScanCode();
#endif
}

QString PushToTalk::keyName(const QKeyEvent* event) {
    const QString name = QKeySequence(event->key()).toString(QKeySequence::NativeText);
    return name.isEmpty() ? "key " + QString::number(nativeKey(event)) : name;
}

bool PushToTalk::eventFilter(QObject* watched, QEvent* event) {
    switch (event->type()) {
        case QEvent::KeyPress:
        case QEvent::KeyRelease: {
            const auto* key = static_cast<QKeyEvent*>(event);
            if (!key->isAutoRepeat() && key_ && nativeKey(key) == *key_)
                setHeld(event->type() == QEvent::KeyPress);
            break;
        }
        case QEvent::ApplicationStateChange:
            // The release would go to whichever app is focused next.
            if (qApp->applicationState() != Qt::ApplicationActive) setHeld(false);
            break;
        default:
            break;
    }
    return QObject::eventFilter(watched, event);
}

void PushToTalk::poll() {
    if (!key_) return;
#if defined(Q_OS_WIN)
    setHeld(GetAsyncKeyState(static_cast<int>(*key_)) & 0x8000);
#elif defined(Q_OS_MACOS)
    setHeld(CGEventSourceKeyState(kCGEventSourceStateHIDSystemState,
                                  static_cast<CGKeyCode>(*key_)));
#elif defined(DISQUISITION_HAVE_XCB)
    std::unique_ptr<xcb_query_keymap_reply_t, decltype(&std::free)> keymap(
        xcb_query_keymap_reply(x11_, xcb_query_keymap(x11_), nullptr), &std::free);
    if (keymap && *key_ < 256) setHeld(keymap->keys[*key_ / 8] & (1 << (*key_ % 8)));
#endif
}

void PushToTalk::setHeld(bool held) {
    if (held_ == held) return;
    held_ = held;
    emit heldChanged(held);
}

PushToTalkKeyButton::PushToTalkKeyButton(QWidget* parent) : QPushButton(parent) {
    // macOS buttons do not take focus on click by default, and the key
    // press arrives through focus.
    setFocusPolicy(Qt::StrongFocus);
    setAutoDefault(false);
    showKey();
    connect(this, &QPushButton::clicked, this, [this] {
        capturing_ = true;
        setText("press a key…");
        setFocus();
    });
}

void PushToTalkKeyButton::setKey(std::optional<quint32> key, const QString& name) {
    key_ = key;
    name_ = name;
    showKey();
}

bool PushToTalkKeyButton::event(QEvent* event) {
    // Keys bound to shortcuts, such as Cmd+, would otherwise never arrive.
    if (capturing_ && event->type() == QEvent::ShortcutOverride) {
        event->accept();
        return true;
    }
    return QPushButton::event(event);
}

void PushToTalkKeyButton::keyPressEvent(QKeyEvent* event) {
    if (!capturing_) {
        QPushButton::keyPressEvent(event);
        return;
    }
    if (event->key() != Qt::Key_Escape) {
        key_ = PushToTalk::nativeKey(event);
        name_ = PushToTalk::keyName(event);
    }
    capturing_ = false;
    showKey();
}

void PushToTalkKeyButton::focusOutEvent(QFocusEvent* event) {
    capturing_ = false;
    showKey();
    QPushButton::focusOutEvent(event);
}

void PushToTalkKeyButton::showKey() {
    setText(key_ ? name_ : "choose key");
}
