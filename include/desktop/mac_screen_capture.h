#pragma once

#include <QImage>
#include <QList>
#include <QString>

#include <functional>

#include "desktop/share_source.h"

// Native macOS capture of a display or a single window. The implementation
// owns its ScreenCaptureKit stream.
class MacScreenCapture {
public:
    MacScreenCapture(std::function<void(QImage)> frame, std::function<void(QString)> error);
    ~MacScreenCapture();
    void startDisplay(quint32 displayId);
    void startWindow(quint32 windowId);
    void stop();
    bool running() const;

private:
    struct State;
    State* state_;
};

// Lists displays and other applications' windows. `done` runs on a
// ScreenCaptureKit thread, with an empty list if listing fails.
void listMacShareSources(std::function<void(QList<ShareSource>)> done);
