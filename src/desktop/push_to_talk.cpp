#include "desktop/push_to_talk.h"

#include <QGuiApplication>
#include <QKeyEvent>
#include <QKeySequence>

#if defined(Q_OS_WIN)
#    include <qt_windows.h>
#elif defined(Q_OS_MACOS)
#    include <CoreGraphics/CoreGraphics.h>
#elif defined(DISQUISITION_HAVE_XCB)
#    include <xcb/xcb.h>
#    include <cstdlib>
#    include <memory>
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
