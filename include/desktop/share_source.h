#pragma once

#include <QList>
#include <QPointer>
#include <QRect>
#include <QScreen>
#include <QSize>
#include <QString>
#include <functional>

#if !defined(Q_OS_MACOS)
#include <QCapturableWindow>
#endif

// Something the user can share: a whole screen or a single application window.
struct ShareSource {
    enum class Kind { Screen, Window };

    Kind kind = Kind::Screen;
    QString title;  // "Screen 1 (main)", or the window's title
    QString detail; // the screen's size, or the window's application when known
#if defined(Q_OS_MACOS)
    quint32 nativeId = 0; // CGDirectDisplayID or CGWindowID
#else
    QPointer<QScreen> screen;
    QCapturableWindow window;
    quintptr nativeId = 0; // HWND or X11 window, when its title identifies it uniquely
#endif
};

// Frames are scaled down to fit maxSize. Each encoded frame must fit in
// kMaxScreenChunks, which keeps resolutions above 1440p out.
struct StreamQuality {
    QString label;
    QSize maxSize;
    int fps = 30;
};

inline const QList<StreamQuality>& streamQualities() {
    static const QList<StreamQuality> qualities {
        {"720p 30fps", {1280, 720}, 30},
        {"1080p 30fps", {1920, 1080}, 30},
        {"1080p 60fps", {1920, 1080}, 60},
        {"1440p 60fps", {2560, 1440}, 60},
    };
    return qualities;
}
constexpr int kDefaultStreamQuality = 1;

// Lists the windows and screens that can be shared, leaving out this app's own
// windows. `done` runs on the UI thread: straight away, or on macOS, where
// listing is asynchronous, after this returns.
void listShareSources(std::function<void(QList<ShareSource>)> done);

// In Qt desktop coordinates. Empty for a hidden/closed window or a compositor
// that does not expose its position, such as Wayland.
QRect shareSourceGeometry(const ShareSource& source);
bool shareSourceSupportsOverlay(const ShareSource& source);

#if !defined(Q_OS_MACOS)
// The process that owns a shared window, or 0 when it cannot be told, such as
// on Wayland or for a window whose title is not unique.
qint64 shareSourceProcessId(const ShareSource& source);
#endif
