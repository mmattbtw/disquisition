#pragma once

#include <QImage>
#include <QList>
#include <QString>

#include <functional>

#include "desktop/share_source.h"
#include "desktop/system_audio_capture.h"

// Native macOS capture of a display or a single window. The implementation
// owns its ScreenCaptureKit stream. When `audio` is set, the stream captures
// the share's sound too, leaving out this app and its child processes.
class MacScreenCapture {
public:
    MacScreenCapture(std::function<void(QImage)> frame, std::function<void(QString)> error,
                     ShareAudioCallback audio = {});
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
QRect macShareSourceGeometry(const ShareSource& source);
