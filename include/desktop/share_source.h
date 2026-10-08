#pragma once

#include <QList>
#include <QPointer>
#include <QScreen>
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
#endif
};

// Lists the windows and screens that can be shared, leaving out this app's own
// windows. `done` runs on the UI thread: straight away, or on macOS, where
// listing is asynchronous, after this returns.
void listShareSources(std::function<void(QList<ShareSource>)> done);
