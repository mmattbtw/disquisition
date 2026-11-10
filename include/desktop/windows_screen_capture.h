#pragma once

#include <QImage>
#include <QString>

#include <functional>
#include <memory>

// Native Windows Graphics Capture of a monitor or a single window. Qt's own
// capture keeps the session's default minimum update interval of 16 ms, which
// on a 60 Hz display skips every other refresh and caps sharing at 30 fps.
// Frames are copied out on a capture thread and handed to `frame` there.
class WindowsScreenCapture {
public:
    WindowsScreenCapture(std::function<void(QImage)> frame, std::function<void(QString)> error,
                         int fps);
    ~WindowsScreenCapture();
    // Return false when native capture is unavailable, so the caller can fall back.
    bool startMonitor(quintptr monitor);
    bool startWindow(quintptr window);
    void stop();
    bool running() const;

private:
    struct State;
    std::unique_ptr<State> state_;
};
