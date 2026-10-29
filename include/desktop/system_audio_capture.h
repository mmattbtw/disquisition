#pragma once

#include <QString>

#include <functional>
#include <memory>

#include "desktop/share_source.h"

// A share's sound is captured as 48 kHz stereo, in interleaved floats.
constexpr int kShareAudioRate = 48000;
constexpr int kShareAudioChannels = 2;
using ShareAudioCallback = std::function<void(const float* interleaved, int frames)>;

// Captures the sound that goes with a share. Sharing a screen takes
// everything the computer plays except this app and its child processes, so
// voice chat is never sent back to the people in it. Sharing a window takes
// only that window's application.
//
// Windows uses WASAPI process loopback and Linux uses PulseAudio or PipeWire.
// macOS captures sound together with the picture in MacScreenCapture instead.
// Elsewhere start() reports that sound is unsupported.
class SystemAudioCapture {
public:
    SystemAudioCapture();
    ~SystemAudioCapture();
    SystemAudioCapture(const SystemAudioCapture&) = delete;
    SystemAudioCapture& operator=(const SystemAudioCapture&) = delete;

    // Calls `samples` on a capture thread until stop(). Returns false and sets
    // `error` when the source's sound cannot be captured. A window whose
    // application cannot be identified gets no sound rather than the whole
    // computer's, which the user did not choose.
    bool start(const ShareSource& source, ShareAudioCallback samples, QString& error);
    // Returns once `samples` will not be called again.
    void stop();

private:
    struct State;
    std::unique_ptr<State> state_;
};
