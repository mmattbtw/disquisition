#pragma once

#include "console/session.h"
#include <3ds.h>

#include <atomic>

namespace handheld {

// Shared with the UI thread. Sleep/HOME stops sampling synchronously and keeps
// a concurrent voice start from restarting it; the keyboard pauses sending.
struct MicGate {
    LightLock lock;
    bool suspended = false;
    std::atomic<bool> paused{false};
    MicGate() { LightLock_Init(&lock); }
    void suspend() {
        LightLock_Lock(&lock);
        suspended = true;
        MICU_StopSampling();
        LightLock_Unlock(&lock);
    }
    void resume() {
        LightLock_Lock(&lock);
        suspended = false;
        LightLock_Unlock(&lock);
    }
};
MicGate& micGate();

class Audio {
public:
    ~Audio() { stop(); }
    bool start(std::string& error);
    void stop();
    void tick(console::Session& session, std::uint64_t now);
    bool running() const { return micReady_; }
    console::Mixer mixer;
private:
    static constexpr unsigned kMicSize = 0x10000;
    u8* microphone_ = nullptr;
    s16* playback_ = nullptr;
    ndspWaveBuf waves_[3]{};
    bool dspReady_ = false;
    bool micReady_ = false;
    bool wasTransmitting_ = false;
    bool wasDeafened_ = false;
    unsigned cursor_ = 0;
    std::uint64_t lastCapture_ = 0;
    std::uint64_t nextPlayback_ = 0;
    console::Resampler resampler_;
};

} // namespace handheld
