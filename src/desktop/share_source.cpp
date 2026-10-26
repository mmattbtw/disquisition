#include "desktop/share_source.h"

#include <QGuiApplication>
#include <QHash>
#include <QWindow>
#include <cstdlib>
#include <cstring>
#include <memory>

#if defined(Q_OS_WIN)
// clang-format off: DWM declarations require Windows types first.
#    include <qt_windows.h>
#    include <dwmapi.h>
// clang-format on
#elif defined(DISQUISITION_HAVE_XCB)
#    include <xcb/xcb.h>
#endif

#if defined(Q_OS_MACOS)
#include "desktop/mac_screen_capture.h"
#else
#include <QWindowCapture>
#endif

namespace {

#if defined(DISQUISITION_HAVE_XCB)
struct XConnection {
    xcb_connection_t* connection = xcb_connect(nullptr, nullptr);
    ~XConnection() { xcb_disconnect(connection); }
    bool valid() const { return !xcb_connection_has_error(connection); }
    xcb_window_t root() const {
        return xcb_setup_roots_iterator(xcb_get_setup(connection)).data->root;
    }
    xcb_atom_t atom(const char* name) const {
        const auto cookie = xcb_intern_atom(connection, true,
                                            static_cast<uint16_t>(std::strlen(name)), name);
        std::unique_ptr<xcb_intern_atom_reply_t, decltype(&std::free)> reply(
            xcb_intern_atom_reply(connection, cookie, nullptr), &std::free);
        return reply ? reply->atom : XCB_ATOM_NONE;
    }
    auto property(xcb_window_t window, xcb_atom_t name, xcb_atom_t type) const {
        return std::unique_ptr<xcb_get_property_reply_t, decltype(&std::free)>(
            xcb_get_property_reply(
                connection, xcb_get_property(connection, false, window, name, type, 0, 65536),
                nullptr),
            &std::free);
    }
};
#endif

#if !defined(Q_OS_MACOS)
// Qt keeps capturable window IDs private. Match the native title at picker
// time, refusing ambiguous titles rather than overlaying the wrong window.
QHash<QString, quintptr> nativeWindows() {
    QHash<QString, quintptr> windows;
#    if defined(Q_OS_WIN)
    EnumWindows(
        [](HWND window, LPARAM data) -> BOOL {
            auto& entries = *reinterpret_cast<QHash<QString, quintptr>*>(data);
            if (!IsWindowVisible(window))
                return TRUE;
            const int length = GetWindowTextLengthW(window);
            if (!length)
                return TRUE;
            std::wstring title(static_cast<std::size_t>(length) + 1, L'\0');
            const int copied = GetWindowTextW(window, title.data(), length + 1);
            const QString key = QString::fromWCharArray(title.data(), copied).trimmed();
            entries[key] = entries.contains(key) ? 0 : reinterpret_cast<quintptr>(window);
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&windows));
#    elif defined(DISQUISITION_HAVE_XCB)
    if (QGuiApplication::platformName() != "xcb")
        return windows;
    XConnection x;
    if (!x.valid())
        return windows;
    const auto list = x.property(x.root(), x.atom("_NET_CLIENT_LIST"), XCB_ATOM_WINDOW);
    if (!list || list->format != 32)
        return windows;
    const auto* ids = static_cast<xcb_window_t*>(xcb_get_property_value(list.get()));
    const int count = xcb_get_property_value_length(list.get()) / sizeof(xcb_window_t);
    const xcb_atom_t titleAtom = x.atom("_NET_WM_NAME");
    for (int i = 0; i < count; ++i) {
        auto title = x.property(ids[i], titleAtom, XCB_GET_PROPERTY_TYPE_ANY);
        bool utf8 = true;
        if (!title || title->format != 8 || !xcb_get_property_value_length(title.get())) {
            title = x.property(ids[i], XCB_ATOM_WM_NAME, XCB_GET_PROPERTY_TYPE_ANY);
            utf8 = false;
        }
        if (!title || title->format != 8)
            continue;
        const char* data = static_cast<const char*>(xcb_get_property_value(title.get()));
        const int length = xcb_get_property_value_length(title.get());
        const QString key = (utf8 ? QString::fromUtf8(data, length)
                                  : QString::fromLocal8Bit(data, length))
                                .trimmed();
        if (!key.isEmpty())
            windows[key] = windows.contains(key) ? 0 : ids[i];
    }
#    endif
    return windows;
}
#endif

#if !defined(Q_OS_MACOS)
QList<ShareSource> screenSources() {
    QList<ShareSource> sources;
    const QScreen* primary = QGuiApplication::primaryScreen();
    const QList<QScreen*> screens = QGuiApplication::screens();
    for (qsizetype i = 0; i < screens.size(); ++i) {
        QScreen* screen = screens[i];
        const QSize pixels = screen->size() * screen->devicePixelRatio();
        ShareSource source;
        source.kind = ShareSource::Kind::Screen;
        source.title = QString("Screen %1").arg(i + 1) + (screen == primary ? " (main)" : "");
        source.detail = QString("%1x%2").arg(pixels.width()).arg(pixels.height());
        source.screen = screen;
        sources.append(source);
    }
    return sources;
}

QList<ShareSource> windowSources() {
    // Qt does not say which windows belong to this process, so match titles.
    // Sharing one of them would only show viewers their own stream.
    QStringList ownTitles;
    for (const QWindow* window : QGuiApplication::topLevelWindows()) {
        if (window->isVisible() && !window->title().isEmpty()) {
            ownTitles.append(window->title());
        }
    }

    QList<ShareSource> sources;
    const auto native = nativeWindows();
    for (const QCapturableWindow& window : QWindowCapture::capturableWindows()) {
        const QString title = window.description().trimmed();
        if (!window.isValid() || title.isEmpty() || ownTitles.contains(title)) {
            continue;
        }
        ShareSource source;
        source.kind = ShareSource::Kind::Window;
        source.title = title;
        source.window = window;
        source.nativeId = native.value(title);
        sources.append(source);
    }
    return sources;
}
#endif

} // namespace

void listShareSources(std::function<void(QList<ShareSource>)> done) {
#if defined(Q_OS_MACOS)
    listMacShareSources([done = std::move(done)](QList<ShareSource> sources) {
        QMetaObject::invokeMethod(QGuiApplication::instance(),
                                  [done, sources = std::move(sources)] { done(sources); });
    });
#else
    done(windowSources() + screenSources());
#endif
}

bool shareSourceSupportsOverlay(const ShareSource& source) {
#if defined(Q_OS_MACOS)
    Q_UNUSED(source);
    return true;
#elif defined(Q_OS_WIN)
    return source.kind == ShareSource::Kind::Screen || source.nativeId != 0;
#elif defined(DISQUISITION_HAVE_XCB)
    return QGuiApplication::platformName() == "xcb" &&
           (source.kind == ShareSource::Kind::Screen || source.nativeId != 0);
#else
    Q_UNUSED(source);
    return false;
#endif
}

QRect shareSourceGeometry(const ShareSource& source) {
#if defined(Q_OS_MACOS)
    return macShareSourceGeometry(source);
#else
    if (source.kind == ShareSource::Kind::Screen)
        return source.screen ? source.screen->geometry() : QRect();
    if (!source.nativeId || !source.window.isValid())
        return {};
#    if defined(Q_OS_WIN)
    const HWND window = reinterpret_cast<HWND>(source.nativeId);
    if (!IsWindow(window) || !IsWindowVisible(window) || IsIconic(window))
        return {};
    RECT bounds;
    if (FAILED(
            DwmGetWindowAttribute(window, DWMWA_EXTENDED_FRAME_BOUNDS, &bounds, sizeof(bounds))) &&
        !GetWindowRect(window, &bounds))
        return {};
    // Native window bounds use physical pixels; Qt uses logical pixels on
    // each monitor. A temporary foreign QWindow supplies Qt's conversion.
    std::unique_ptr<QWindow> foreign(QWindow::fromWinId(source.nativeId));
    if (!foreign || !foreign->screen())
        return {};
    MONITORINFO monitor{sizeof(MONITORINFO)};
    if (!GetMonitorInfoW(MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST), &monitor))
        return {};
    const qreal scale = foreign->devicePixelRatio();
    const QPoint origin = foreign->screen()->geometry().topLeft();
    return QRect(origin + QPoint(qRound((bounds.left - monitor.rcMonitor.left) / scale),
                                 qRound((bounds.top - monitor.rcMonitor.top) / scale)),
                 QSize(qRound((bounds.right - bounds.left) / scale),
                       qRound((bounds.bottom - bounds.top) / scale)));
#    elif defined(DISQUISITION_HAVE_XCB)
    if (QGuiApplication::platformName() != "xcb")
        return {};
    XConnection x;
    if (!x.valid())
        return {};
    std::unique_ptr<xcb_get_window_attributes_reply_t, decltype(&std::free)> attributes(
        xcb_get_window_attributes_reply(
            x.connection, xcb_get_window_attributes(x.connection, source.nativeId), nullptr),
        &std::free);
    if (!attributes || attributes->map_state != XCB_MAP_STATE_VIEWABLE)
        return {};
    // Qt converts the foreign window's geometry to logical desktop pixels.
    std::unique_ptr<QWindow> foreign(QWindow::fromWinId(source.nativeId));
    return foreign ? foreign->geometry() : QRect();
#    else
    return {};
#    endif
#endif
}
