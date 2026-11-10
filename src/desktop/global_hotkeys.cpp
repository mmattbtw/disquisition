#include "desktop/global_hotkeys.h"

#if defined(Q_OS_WIN)
#    include <qt_windows.h>
#    include <utility>

namespace {
const wchar_t* kWindowClass = L"DisquisitionGlobalHotkeys";

LRESULT CALLBACK hotkeyProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    if (message == WM_HOTKEY) {
        if (auto* owner = reinterpret_cast<GlobalHotkeys*>(GetWindowLongPtrW(window, GWLP_USERDATA)))
            emit owner->activated(static_cast<int>(wParam));
        return 0;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

UINT virtualKey(Qt::Key key) {
    if (key >= Qt::Key_0 && key <= Qt::Key_9)
        return '0' + (key - Qt::Key_0);
    if (key >= Qt::Key_A && key <= Qt::Key_Z)
        return 'A' + (key - Qt::Key_A);
    if (key == Qt::Key_Escape)
        return VK_ESCAPE;
    return 0;
}

UINT nativeModifiers(Qt::KeyboardModifiers modifiers) {
    UINT result = MOD_NOREPEAT;
    if (modifiers & Qt::ControlModifier) result |= MOD_CONTROL;
    if (modifiers & Qt::ShiftModifier) result |= MOD_SHIFT;
    if (modifiers & Qt::AltModifier) result |= MOD_ALT;
    if (modifiers & Qt::MetaModifier) result |= MOD_WIN;
    return result;
}
}

GlobalHotkeys::GlobalHotkeys(QObject* parent) : QObject(parent) {
    const HINSTANCE instance = GetModuleHandleW(nullptr);
    WNDCLASSW type{};
    if (!GetClassInfoW(instance, kWindowClass, &type)) {
        type.lpfnWndProc = hotkeyProc;
        type.hInstance = instance;
        type.lpszClassName = kWindowClass;
        RegisterClassW(&type);
    }
    HWND window = CreateWindowExW(0, kWindowClass, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr,
                                  instance, nullptr);
    if (window)
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
    window_ = window;
}

GlobalHotkeys::~GlobalHotkeys() {
    auto* window = static_cast<HWND>(window_);
    if (!window)
        return;
    for (const int id : std::as_const(registered_))
        UnregisterHotKey(window, id);
    SetWindowLongPtrW(window, GWLP_USERDATA, 0);
    DestroyWindow(window);
}

bool GlobalHotkeys::set(int id, bool enabled, Qt::KeyboardModifiers modifiers, Qt::Key key) {
    auto* window = static_cast<HWND>(window_);
    if (!window)
        return false;
    if (enabled == registered_.contains(id))
        return enabled;
    if (!enabled) {
        UnregisterHotKey(window, id);
        registered_.remove(id);
        return false;
    }
    const UINT code = virtualKey(key);
    if (!code || !RegisterHotKey(window, id, nativeModifiers(modifiers), code))
        return false;
    registered_.insert(id);
    return true;
}

#else

GlobalHotkeys::GlobalHotkeys(QObject* parent) : QObject(parent) {}
GlobalHotkeys::~GlobalHotkeys() = default;

bool GlobalHotkeys::set(int id, bool enabled, Qt::KeyboardModifiers modifiers, Qt::Key key) {
    Q_UNUSED(id);
    Q_UNUSED(enabled);
    Q_UNUSED(modifiers);
    Q_UNUSED(key);
    return false;
}

#endif
