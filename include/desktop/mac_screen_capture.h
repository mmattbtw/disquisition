#pragma once

#include <QImage>
#include <QString>

#include <functional>

// Native macOS display capture. The implementation owns its ScreenCaptureKit stream.
class MacScreenCapture {
public:
    MacScreenCapture(std::function<void(QImage)> frame, std::function<void(QString)> error);
    ~MacScreenCapture();
    void start();
    void stop();
    bool running() const;

private:
    struct State;
    State* state_;
};
