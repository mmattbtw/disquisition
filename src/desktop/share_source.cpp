#include "desktop/share_source.h"

#include <QGuiApplication>
#include <QWindow>

#if defined(Q_OS_MACOS)
#include "desktop/mac_screen_capture.h"
#else
#include <QWindowCapture>
#endif

namespace {

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
    for (const QCapturableWindow& window : QWindowCapture::capturableWindows()) {
        const QString title = window.description().trimmed();
        if (!window.isValid() || title.isEmpty() || ownTitles.contains(title)) {
            continue;
        }
        ShareSource source;
        source.kind = ShareSource::Kind::Window;
        source.title = title;
        source.window = window;
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
