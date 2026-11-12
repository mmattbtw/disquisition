#pragma once

#include <QImage>
#include <QString>

#include <functional>
#include <memory>

// Qt's capture keeps Windows' 16 ms minimum update interval, which caps a
// 60 Hz display at 30 fps.
class WindowsScreenCapture {
public:
    WindowsScreenCapture(std::function<void(QImage)> frame, std::function<void(QString)> error,
                         int fps);
    ~WindowsScreenCapture();
    bool startMonitor(quintptr monitor);
    bool startWindow(quintptr window);
    void stop();
    bool running() const;

private:
    struct State;
    std::unique_ptr<State> state_;
};
