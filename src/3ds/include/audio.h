#pragma once

#include "session.h"
#include <3ds.h>

namespace handheld {

class Audio {
public:
    ~Audio() { stop(); }
    bool start(std::string& error);
    void stop();
    void tick(Session& session, std::uint64_t now);
    bool running() const { return micReady_; }
    Mixer mixer;
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
    Resampler resampler_;
};

} // namespace handheld
